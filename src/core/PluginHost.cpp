#include "PluginHost.h"
#include "../app/Paths.h"
#include "../storage/JsonStore.h"
#include "../platform/Archive.h"

#include <nlohmann/json.hpp>
#include <windows.h>

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdio>
#include <deque>
#include <mutex>

namespace hs {

// 每个加载插件的运行时状态
struct PluginHost::PluginInstance {
    HMODULE dll = nullptr;
    const hs_plugin_api_vtable* api = nullptr;
    void* userData = nullptr;
    PluginInfo info;
};

namespace {

// ---------------------------------------------------------------------------
// 插件日志环形缓冲 (hs_log 落地 + 动作结果反馈)。宿主 vtable 是全局单例,
// 用 thread_local 记录"当前正在执行的插件"来归属日志 (宿主调用插件入口处打标)。
// ---------------------------------------------------------------------------
struct PluginLogEntry {
    std::string plugin;
    std::string level;
    std::string msg;
};
std::mutex g_logMx;
std::deque<PluginLogEntry> g_logRing;          // 上限 200 条, 超出丢最旧
thread_local std::string t_logPlugin;

struct PluginScope {                           // RAII: 宿主进入插件代码期间打标
    explicit PluginScope(const std::string& id) { t_logPlugin = id; }
    ~PluginScope() { t_logPlugin.clear(); }
    PluginScope(const PluginScope&) = delete;
    PluginScope& operator=(const PluginScope&) = delete;
};

// 取 [from, end) 中属于 plugin 的日志尾部, 供动作结果展示 (≤8 行 / ≤600 字节)。
// 含 "@@DATA@@" 标记的行是插件的结构化结果 (HSPP v1.4), 抽出最后一行放到
// *dataOut (原文, 调用方自行 json::parse), 不计入尾部文本。
std::string logTailOf(size_t from, const std::string& plugin, std::string* dataOut = nullptr) {
    std::lock_guard<std::mutex> lk(g_logMx);
    std::vector<std::string> lines;
    if (dataOut) dataOut->clear();
    for (size_t i = from; i < g_logRing.size(); ++i) {
        if (g_logRing[i].plugin != plugin) continue;
        size_t mk = g_logRing[i].msg.find("@@DATA@@");
        if (mk != std::string::npos) {
            if (dataOut) *dataOut = g_logRing[i].msg.substr(mk + 8);
            continue;
        }
        lines.push_back("[" + g_logRing[i].level + "] " + g_logRing[i].msg);
        if (lines.size() > 32) lines.erase(lines.begin());   // 防爆, 最终仍裁 8 行
    }
    std::string out;
    size_t start = lines.size() > 8 ? lines.size() - 8 : 0;
    for (size_t i = start; i < lines.size(); ++i) {
        if (!out.empty()) out += "\n";
        out += lines[i];
    }
    if (out.size() > 600) out = out.substr(out.size() - 600);
    return out;
}

// get_ui_entries 的 sink 是纯函数指针 (不能捕获), 通过 ctx 同时传
// 输出向量与插件 ABI 版本 (sidebar 字段仅 ABI v2+ 存在, v1 越界读)
struct UiEntryCtx {
    std::vector<hs::PluginInfo::UiEntry>* out;
    int abiVersion;
};

void collectUiEntry(const hs_ui_entry* e, void* ctx) {
    if (!e) return;
    auto* c = static_cast<UiEntryCtx*>(ctx);
    hs::PluginInfo::UiEntry ue;
    ue.id = e->id ? e->id : "";
    ue.title = e->title ? e->title : "";
    ue.icon = e->icon ? e->icon : "";
    ue.jsonSchema = e->json_schema ? e->json_schema : "";
    ue.defaultConfig = e->default_config ? e->default_config : "";
    ue.actions = e->actions ? e->actions : "";
    ue.sidebar = (c->abiVersion >= 2 && e->sidebar != 0);
    c->out->push_back(std::move(ue));
}

// 宿主 API vtable 实现 (函数指针绑定到静态实现)
void hsLog(hs_log_level lvl, hs_str tag, hs_str msg) {
    const char* l = "INFO";
    switch (lvl) { case HS_LOG_DEBUG: l="DEBUG"; break; case HS_LOG_INFO: l="INFO"; break;
                   case HS_LOG_WARN: l="WARN"; break; case HS_LOG_ERROR: l="ERROR"; break; }
    std::lock_guard<std::mutex> lk(g_logMx);
    PluginLogEntry e;
    e.plugin = t_logPlugin;                     // 空 = 非动作上下文 (create/事件期)
    e.level = l;
    if (msg) e.msg = msg;
    if (tag && !e.msg.empty() && e.msg.find(std::string(tag)) != 0) e.msg = std::string(tag) + ": " + e.msg;
    g_logRing.push_back(std::move(e));
    while (g_logRing.size() > 200) g_logRing.pop_front();
}

hs_i32 hsConfigGet(hs_str key, hs_str* out) {
    // hermesRoot 特殊处理: 设置里存的是相对便携包根的相对路径(如 .\Hermes),
    // 插件需要的是解析后的绝对路径 (用于定位文件)。
    if (key && strcmp(key, "hermesRoot") == 0) {
        fs::path resolved = Paths::resolveRootFromSettings();
        if (!resolved.empty()) {
            *out = _strdup(("\"" + resolved.string() + "\"").c_str());
            return 0;
        }
    }
    // 先查 app.json, 再查 settings.json (启动器核心配置含 hermesRoot/port/proxy 等)
    auto cfg = JsonStore::load(Paths::studioDir() / "app.json");
    if (cfg && cfg->contains(key)) {
        *out = _strdup(cfg->at(key).dump().c_str());
        return 0;
    }
    auto st = JsonStore::load(Paths::studioDir() / "settings.json");
    if (st) {
        // 支持点路径: hermesRoot / runtime.hermesRoot / network.host 等
        if (st->contains(key)) { *out = _strdup(st->at(key).dump().c_str()); return 0; }
        auto find = [&](const json& root, const std::string& k)->bool {
            for (auto it = root.begin(); it != root.end(); ++it) {
                if (it.key() == k) { *out = _strdup(it.value().dump().c_str()); return true; }
                if (it.value().is_object()) {
                    std::string nested;
                    for (auto n = it.value().begin(); n != it.value().end(); ++n)
                        if (n.key() == k) { nested = n.value().dump(); break; }
                    if (!nested.empty()) { *out = _strdup(nested.c_str()); return true; }
                }
            }
            return false;
        };
        if (find(*st, std::string(key))) return 0;
    }
    return -1;
}
hs_i32 hsConfigSet(hs_str key, hs_str val) {
    auto path = Paths::studioDir() / "app.json";
    auto cfg = JsonStore::load(path).value_or(json::object());
    if (val) { try { cfg[key] = json::parse(val); } catch (...) { return -1; } }
    else cfg.erase(key);
    auto r = JsonStore::save(path, cfg);
    return r.ok() ? 0 : -1;
}
void hsStringFree(hs_str s) { if (s) free((void*)s); }

void hsRegisterCheck(hs_str, hs_str, hs_i32, hs_str) {}
void hsRegisterProviderType(hs_str, hs_str) {}
hs_i32 hsModelsGet(hs_str* out) {
    auto m = JsonStore::load(Paths::studioDir() / "models.json");
    if (!m) return -1;
    *out = _strdup(m->dump().c_str());
    return 0;
}
hs_i32 hsModelsSet(hs_str jsonStr) {
    if (!jsonStr) return -1;
    try {
        auto j = json::parse(jsonStr);
        auto r = JsonStore::save(Paths::studioDir() / "models.json", j);
        return r.ok() ? 0 : -1;
    } catch (...) { return -1; }
}
void hsProgress(hs_str, hs_i32, hs_str) {}

const hs_host_api_vtable g_hostApi = {
    1,                     // api_version
    hsLog,
    hsConfigGet, hsConfigSet, hsStringFree,
    hsRegisterCheck,
    hsRegisterProviderType,
    hsModelsGet, hsModelsSet,
    hsProgress,
};

fs::path stateFile() { return Paths::pluginDir() / "state.json"; }

} // namespace

PluginHost::PluginHost() {
    loadState();
}
PluginHost::~PluginHost() { unloadAll(); }

std::vector<PluginInfo> PluginHost::discover() const {
    std::vector<PluginInfo> out;
    fs::path base = Paths::pluginDir();
    std::error_code ec;
    if (!fs::is_directory(base, ec)) return out;
    for (fs::directory_iterator it(base, ec), end; it != end; ++it) {
        if (!it->is_directory(ec)) continue;
        fs::path idir = it->path();
        fs::path dll  = idir / "plugin.dll";
        if (!fs::exists(dll, ec)) continue;

        PluginInfo pi;
        pi.id = idir.filename().string();
        pi.dir = idir;
        pi.dllPath = dll;

        // 读 plugin.json + 协议校验 (docs/PLUGIN-PROTOCOL.md §1.1)
        // 违规不加载, loadError 显示给用户
        auto jm = JsonStore::load(idir / "plugin.json");
        if (!jm) {
            pi.enabled = false;
            pi.loadError = "manifest 缺失或不可解析 (plugin.json)";
            out.push_back(std::move(pi));
            continue;
        }
        if (!jm->contains("name") || !(*jm)["name"].is_string() ||
            !jm->contains("version") || !(*jm)["version"].is_string() ||
            !jm->contains("api") || !(*jm)["api"].is_number()) {
            pi.enabled = false;
            pi.loadError = "manifest 缺少必填字段 (name/version/api)";
            out.push_back(std::move(pi));
            continue;
        }
        std::string mid = jm->value("id", std::string{});
        if (mid.empty()) mid = pi.id;   // 容错: 老清单可省 id, 默认取目录名
        if (mid != pi.id) {
            pi.enabled = false;
            pi.loadError = "manifest id (" + mid + ") 与目录名 (" + pi.id + ") 不一致";
            out.push_back(std::move(pi));
            continue;
        }
        int api = (*jm)["api"].get<int>();
        if (api != 1) {
            pi.enabled = false;
            pi.loadError = "manifest api 版本不受支持 (需要 1, 实际 " + std::to_string(api) + ")";
            out.push_back(std::move(pi));
            continue;
        }
        pi.name = jm->value("name", pi.id);
        pi.version = jm->value("version", std::string{});
        pi.description = jm->value("description", std::string{});
        pi.author = jm->value("author", std::string{});
        pi.quick = jm->value("quick", true);   // HSPP v1.3: 清单可声明不出现在快捷区

        // 合并启停状态 + 侧边栏偏好 (缺省: 启用 + 显示)
        auto itState = state_.find(pi.id);
        pi.enabled = (itState == state_.end()) ? true : itState->second;
        auto itSidebar = sidebarState_.find(pi.id);
        pi.sidebarEnabled = (itSidebar == sidebarState_.end()) ? true : itSidebar->second;

        out.push_back(std::move(pi));
    }
    return out;
}

std::vector<PluginInfo> PluginHost::allPlugins() const {
    auto out = discover();
    // 合并本次进程的真实加载状态: discover 产生的是全新副本
    // (loaded=false/无 uiEntries), 曾致页面把已加载插件显示成"加载失败"
    for (auto& p : out) {
        for (auto& a : attempted_) {
            if (a.id == p.id) {
                p.loaded = a.loaded;
                p.loadError = a.loadError;
                p.uiEntries = a.uiEntries;
                break;
            }
        }
    }
    return out;
}

void PluginHost::loadState() {
    state_.clear();
    sidebarState_.clear();
    switchState_.clear();
    auto j = JsonStore::load(stateFile());
    if (!j || !j->is_object()) return;
    auto it = j->find("plugins");
    if (it == j->end() || !it->is_object()) return;
    for (auto& [k, v] : it->items()) {
        if (!v.is_object()) continue;
        if (v.contains("enabled") && v["enabled"].is_boolean())
            state_[k] = v["enabled"].get<bool>();
        if (v.contains("sidebar") && v["sidebar"].is_boolean())
            sidebarState_[k] = v["sidebar"].get<bool>();
        if (v.contains("switches") && v["switches"].is_object()) {
            for (auto& [a, sv] : v["switches"].items())
                if (sv.is_boolean()) switchState_[k][a] = sv.get<bool>();
        }
    }
}

void PluginHost::saveState() {
    json j;
    j["schema"] = 1;
    j["plugins"] = json::object();
    for (auto& [k, v] : state_) j["plugins"][k]["enabled"] = v;
    for (auto& [k, v] : sidebarState_) j["plugins"][k]["sidebar"] = v;
    for (auto& [k, sw] : switchState_)
        for (auto& [a, sv] : sw) j["plugins"][k]["switches"][a] = sv;
    JsonStore::save(stateFile(), j);
}

bool PluginHost::setEnabled(const std::string& id, bool enabled, std::string* err) {
    bool found = false;
    for (auto& p : discover()) if (p.id == id) { found = true; break; }
    if (!found) {
        if (err) *err = "插件不存在: " + id;
        return false;
    }
    state_[id] = enabled;
    saveState();
    return true;
}

bool PluginHost::setSidebar(const std::string& id, bool visible, std::string* err) {
    bool found = false;
    for (auto& p : discover()) if (p.id == id) { found = true; break; }
    if (!found) {
        if (err) *err = "插件不存在: " + id;
        return false;
    }
    sidebarState_[id] = visible;
    saveState();
    return true;
}

bool PluginHost::removePlugin(const std::string& id, std::string* err) {
    bool found = false;
    fs::path dir;
    for (auto& p : discover()) if (p.id == id) { found = true; dir = p.dir; break; }
    if (!found) {
        if (err) *err = "插件不存在: " + id;
        return false;
    }
    // 若已加载先卸载
    for (auto it = instances_.begin(); it != instances_.end(); ++it) {
        if ((*it)->info.id == id) {
            if ((*it)->api->destroy && (*it)->userData) (*it)->api->destroy((*it)->userData);
            if ((*it)->dll) FreeLibrary((*it)->dll);
            instances_.erase(it);
            loaded_.erase(std::remove_if(loaded_.begin(), loaded_.end(),
                [&](const PluginInfo& p){ return p.id == id; }), loaded_.end());
            break;
        }
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (ec) { if (err) *err = "删除插件目录失败: " + ec.message(); return false; }
    state_.erase(id);
    sidebarState_.erase(id);
    switchState_.erase(id);
    saveState();
    return true;
}

bool PluginHost::loadOne(PluginInfo& info) {
    HMODULE dll = LoadLibraryW(info.dllPath.wstring().c_str());
    if (!dll) {
        info.loadError = "LoadLibrary 失败: " + std::to_string(GetLastError());
        return false;
    }
    auto entry = (const hs_plugin_api_vtable* (*)(void))GetProcAddress(dll, "hs_plugin_entry");
    if (!entry) {
        info.loadError = "缺少导出 hs_plugin_entry";
        FreeLibrary(dll);
        return false;
    }
    const hs_plugin_api_vtable* api = entry();
    // ABI 兼容区间: 1 (无 sidebar 字段) .. HS_ABI_VERSION(2)。更高 = 拒载
    if (!api || api->abi_version == 0 || api->abi_version > HS_ABI_VERSION) {
        info.loadError = "ABI 版本不匹配 (支持 1.." + std::to_string(HS_ABI_VERSION) + ")";
        FreeLibrary(dll);
        return false;
    }
    info.abiVersion = (int)api->abi_version;
    // 宿主 API 版本由清单 api 字段声明 (discover 阶段已校验), DLL 表只含 abi_version

    auto inst = std::make_unique<PluginInstance>();
    inst->dll = dll;
    inst->api = api;
    inst->info = info;

    if (api->create) {
        hs_i32 rc;
        {
            PluginScope scope(info.id);
            rc = api->create(&g_hostApi, &inst->userData);
        }
        if (rc != 0) {
            info.loadError = "create 失败, code=" + std::to_string(rc);
            FreeLibrary(dll);
            return false;
        }
    }

    // 收集声明式 UI 入口
    if (api->get_ui_entries && inst->userData) {
        UiEntryCtx ctx{ &inst->info.uiEntries, (int)api->abi_version };
        PluginScope scope(info.id);
        api->get_ui_entries(inst->userData, &collectUiEntry, &ctx);
    }

    inst->info.loaded = true;
    loaded_.push_back(inst->info);
    instances_.push_back(std::move(inst));
    return true;
}

void PluginHost::loadAll() {
    attempted_.clear();
    for (auto& pi : discover()) {
        if (!pi.enabled) continue;   // 停用的不加载
        bool ok = loadOne(pi);
        pi.loaded = ok;              // loadOne 把成功副本(含 uiEntries)放入 loaded_
        if (ok && !loaded_.empty()) pi.uiEntries = loaded_.back().uiEntries;
        attempted_.push_back(pi);    // 失败也记录 (loadError 已写入 pi)
    }
}

void PluginHost::reload() {
    unloadAll();
    loadAll();
}

bool PluginHost::dispatch(hs_event ev, const std::string& payload) {
    bool allowed = true;
    for (auto& inst : instances_) {
        if (!inst->api->on_event) continue;
        PluginScope scope(inst->info.id);
        hs_event_result r = inst->api->on_event(inst->userData, ev, payload.c_str());
        if (r == HS_EVENT_VETO) allowed = false;
    }
    return allowed;
}

const std::vector<PluginInfo::UiEntry>* PluginHost::uiEntriesOf(const std::string& id) const {
    for (auto& p : loaded_) {
        if (p.id == id) return &p.uiEntries;
    }
    return nullptr;
}

hs_i32 PluginHost::executeAction(const std::string& pluginId, const std::string& actionId,
                                 const std::string& payload) {
    return executeActionEx(pluginId, actionId, payload).rc;
}

PluginHost::ActionOutcome PluginHost::executeActionEx(const std::string& pluginId,
                                                      const std::string& actionId,
                                                      const std::string& payload) {
    ActionOutcome out;
    for (auto& inst : instances_) {
        if (inst->info.id != pluginId) continue;
        if (!inst->api->execute_action) { out.rc = -2; return out; }   // 插件不支持动作
        if (!inst->userData)            { out.rc = -3; return out; }   // userData 为空
        size_t mark;
        { std::lock_guard<std::mutex> lk(g_logMx); mark = g_logRing.size(); }
        {
            PluginScope scope(pluginId);
            out.rc = inst->api->execute_action(inst->userData, actionId.c_str(), payload.c_str());
        }
        out.logTail = logTailOf(mark, pluginId, &out.data);
        return out;
    }
    out.rc = -1;   // 未找到插件
    return out;
}

bool PluginHost::switchState(const std::string& pluginId, const std::string& actionId,
                             bool declaredDefault) const {
    auto it = switchState_.find(pluginId);
    if (it == switchState_.end()) return declaredDefault;
    auto it2 = it->second.find(actionId);
    return it2 == it->second.end() ? declaredDefault : it2->second;
}

void PluginHost::setSwitchState(const std::string& pluginId, const std::string& actionId, bool v) {
    switchState_[pluginId][actionId] = v;
    saveState();
}

std::map<std::string, std::map<std::string, bool>> PluginHost::allSwitchStates() const {
    return switchState_;
}

bool PluginHost::installFromZip(const fs::path& zipPath, std::string* err) {
    auto fail = [&](const std::string& m) { if (err) *err = m; return false; };
    if (!fs::exists(zipPath)) return fail("安装包不存在: " + zipPath.string());
    fs::path base = Paths::pluginDir();
    std::error_code ec;
    fs::create_directories(base, ec);
    fs::path incoming = base / ("_incoming-" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(incoming, ec);   // 上次残留
    auto extractErr = arc::extractAuto(zipPath, incoming);
    if (extractErr) {
        if (err) *err = "解压失败: " + *extractErr;
        fs::remove_all(incoming, ec);
        return false;
    }

    // 定位 plugin.json: 根目录, 否则唯一含它的单层子目录
    fs::path src = incoming, manifest = incoming / "plugin.json";
    if (!fs::exists(manifest, ec)) {
        fs::path found;
        int dirs = 0;
        for (fs::directory_iterator it(incoming, ec), end; it != end; ++it) {
            if (!it->is_directory(ec)) continue;
            ++dirs;
            if (fs::exists(it->path() / "plugin.json", ec)) { found = it->path(); break; }
        }
        if (found.empty()) {
            fs::remove_all(incoming, ec);
            return fail("安装包内找不到 plugin.json (须在 zip 根目录或唯一子目录内)");
        }
        src = found;
        manifest = found / "plugin.json";
    }

    // 校验清单 (与 discover 同规则: id==目录名, api==1)
    auto jm = JsonStore::load(manifest);
    if (!jm || !jm->contains("name") || !jm->contains("version") || !jm->contains("api")) {
        fs::remove_all(incoming, ec);
        return fail("plugin.json 缺失必填字段 (name/version/api)");
    }
    int api = (*jm)["api"].get<int>();
    if (api != 1) {
        fs::remove_all(incoming, ec);
        return fail("清单 api 版本不受支持 (需要 1, 实际 " + std::to_string(api) + ")");
    }
    std::string id = jm->value("id", src.filename().string());
    if (id.empty() || id.find_first_of("/\\") != std::string::npos) {
        fs::remove_all(incoming, ec);
        return fail("清单 id 非法: " + id);
    }
    fs::path target = base / id;
    if (fs::exists(target, ec)) {
        fs::remove_all(incoming, ec);
        return fail("插件已存在: " + id + " (请先在插件页删除后再安装)");
    }
    // 落位 (同卷 rename, 跨卷兜底 copy+remove)
    fs::create_directories(target.parent_path(), ec);
    fs::rename(src, target, ec);
    if (ec) {
        ec.clear();
        fs::copy(src, target, fs::copy_options::recursive, ec);
        if (ec) {
            fs::remove_all(incoming, ec);
            return fail("落位失败: " + ec.message());
        }
    }
    fs::remove_all(incoming, ec);
    reload();
    return true;
}

void PluginHost::unloadAll() {
    for (auto& inst : instances_) {
        if (inst->api->destroy && inst->userData) {
            PluginScope scope(inst->info.id);
            inst->api->destroy(inst->userData);
        }
        if (inst->dll) FreeLibrary(inst->dll);
    }
    instances_.clear();
    loaded_.clear();
}

} // namespace hs
