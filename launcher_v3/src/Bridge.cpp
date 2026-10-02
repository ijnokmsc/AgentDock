// Bridge.cpp —— WebMessage 桥宿主端 (ADR-007 第四节协议)
//
// 页面 → postMessage({cmd, ...}) → Bridge_HandleJson → 白名单分发
//   快命令 当前线程直接执行; 慢命令 (build/update/preflight/log) 丢 TaskRunner 后台执行。
// 宿主 → 页面: Bridge_PushEvent(json) → WebViews ExecuteScript __hostEvent(json)
//   (任意线程可调, 内部 marshal 回 UI 线程 —— WebView2 是 STA)。
#include "Ui.h"
#include "WebViews.h"
#include "TaskRunner.h"
#include "core/UpdateManager.h"
#include "core/PreflightChecker.h"
#include "core/DshRuntime.h"
#include "core/DshBuilder.h"
#include "core/ProviderPresets.h"
#include "core/AgentSpec.h"
#include "storage/JsonStore.h"
#include "platform/PortScanner.h"
#include "platform/ProxyTester.h"

#include <nlohmann/json.hpp>
#include <atomic>
#include <thread>
#include <fstream>
#include <shellapi.h>
#include <commdlg.h>    // GetOpenFileNameW (插件 zip 安装选包)

namespace hs {

namespace {

DWORD g_uiThread = 0;
std::atomic<bool> g_building{false};
std::atomic<bool> g_buildCancel{false};
std::atomic<bool> g_dshBuilding{false};
std::atomic<bool> g_dshBuildCancel{false};
std::atomic<bool> g_busy{false};

using nlohmann::json;

// ---- 事件推送 (线程安全) ----
// 净化字符串: 移除非法 UTF-8 字节 (OS/子进程可能输出 GBK), 返回合法 UTF-8
std::string SanitizeUtf8(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        unsigned char c = (unsigned char)in[i];
        if (c < 0x80) { out += (char)c; continue; }
        if (c >= 0xC2 && c <= 0xDF && i + 1 < in.size()
            && ((unsigned char)in[i+1] & 0xC0) == 0x80) {
            out += in[i]; out += in[i+1]; i += 1; continue;
        }
        if (c >= 0xE0 && c <= 0xEF && i + 2 < in.size()
            && ((unsigned char)in[i+1] & 0xC0) == 0x80
            && ((unsigned char)in[i+2] & 0xC0) == 0x80) {
            out += in[i]; out += in[i+1]; out += in[i+2]; i += 2; continue;
        }
        if (c >= 0xF0 && c <= 0xF4 && i + 3 < in.size()
            && ((unsigned char)in[i+1] & 0xC0) == 0x80
            && ((unsigned char)in[i+2] & 0xC0) == 0x80
            && ((unsigned char)in[i+3] & 0xC0) == 0x80) {
            out += in[i]; out += in[i+1]; out += in[i+2]; out += in[i+3]; i += 3; continue;
        }
        out += '?';
    }
    return out;
}

void PushEventThreadSafe(std::string json) {
    if (GetCurrentThreadId() != g_uiThread) {
        TaskRunner::Instance().Post([json]() { Bridge_PushEvent(json); });
        return;
    }
    Bridge_PushEvent(json);
}

void PushOp(const std::string& cmd, bool ok, const std::wstring& message) {
    json e;
    e["type"] = "op.result";
    e["cmd"] = cmd;
    e["ok"] = ok;
    e["message"] = WideToUtf8(message);
    PushEventThreadSafe(e.dump());
}

bool NeedCore(const char* cmd) {   // PendingBuild 模式下不可用的命令
    if (g_app.coreReady) return true;
    PushOp(cmd, false, L"便携包尚未构建, 请先在「构建」页完成从0构建");
    return false;
}

// ---- 进程 ----
void HProcStart() {
    if (!NeedCore("proc.start") || g_busy.exchange(true)) return;
    TaskRunner::Instance().Post([]() {
        // 插件 veto (Qt6 行为: HS_EVENT_APP_STARTING 返回 false 阻止启动)
        if (!g_app.plugins.dispatch(HS_EVENT_APP_STARTING)) {
            g_busy = false;
            PushOp("proc.start", false, L"插件阻止了本次启动 (veto)");
            return;
        }
        std::wstring err;
        bool ok = g_app.proc.Start(err);
        g_busy = false;
        if (ok) logSink().Log(LogLevel::Info, L"已请求启动 Hermes");
        else    logSink().Log(LogLevel::Error, L"启动失败: " + err);
        g_app.plugins.dispatch(HS_EVENT_APP_STARTED);
        PushOp("proc.start", ok, ok ? L"正在启动, 就绪后自动切到 Hermes 页" : err);
        App_RefreshStatusUI();
    });
}

void HProcStop() {
    if (!NeedCore("proc.stop") || g_busy.exchange(true)) return;
    TaskRunner::Instance().Post([]() {
        g_app.plugins.dispatch(HS_EVENT_APP_STOPPING);
        g_app.proc.Stop();
        g_busy = false;
        logSink().Log(LogLevel::Info, L"已停止 Hermes");
        PushOp("proc.stop", true, L"已停止");
        App_RefreshStatusUI();
    });
}

void HProcRestart() {
    if (!NeedCore("proc.restart") || g_busy.exchange(true)) return;
    TaskRunner::Instance().Post([]() {
        g_app.proc.Restart();
        g_busy = false;
        PushOp("proc.restart", true, L"正在重启网关...");
        App_RefreshStatusUI();
    });
}

// ---- 构建 ★ ----
void HBuildResolve() {
    TaskRunner::Instance().Post([]() {
        auto info = g_app.builder.resolveLatest("win-x64", {});
        json e;
        e["type"] = "build.remote";
        e["ok"] = info.ok;
        e["message"] = info.message;
        if (info.ok) {
            e["runtime"] = { {"version", info.runtimeVersion}, {"tag", info.runtimeTag},
                             {"size", (double)info.runtimeSize}, {"sha256", info.runtimeSha256},
                             {"url", info.runtimeUrl} };
            e["webUi"] = { {"version", info.webUiVersion}, {"tag", info.webUiTag},
                           {"size", (double)info.webUiSize}, {"sha256", info.webUiSha256},
                           {"url", info.webUiUrl} };
            // 手动下载放置目录 (文件名 = URL 原文件名, useCache 会直接命中)
            e["downloadDir"] = WideToUtf8(hs::Paths::downloadDir().wstring());
        }
        logSink().Log(LogLevel::Info, Utf8ToWide("远端版本解析: " + info.message));
        PushEventThreadSafe(e.dump());
    });
}

void HBuildStart(const json& args) {
    if (g_building.exchange(true)) { PushOp("build.start", false, L"构建已在进行中"); return; }
    TaskRunner::Instance().Post([args]() {
        hs::BuildOptions opts;
        if (args.contains("targetDir") && args["targetDir"].is_string()) {
            std::string t = args["targetDir"];
            if (!t.empty()) {
                std::wstring wt = Utf8ToWide(t);
                // 支持 .\Hermes 等相对写法 (相对便携包根/exe 所在目录)
                bool isAbs = (wt.size() >= 2 && wt[1] == L':') || wt[0] == L'\\' || wt[0] == L'/';
                opts.targetDir = isAbs ? wt : fs::path(g_app.exeDirW) / wt;
            }
        }
        if (args.contains("useCache")) opts.useCache = args["useCache"].get<bool>();
        if (args.contains("mirror") && args["mirror"].is_string())
            opts.githubMirror = args["mirror"].get<std::string>();
        g_buildCancel = false;
        logSink().Log(LogLevel::Info, L"开始构建便携包 (runtime + web-ui)...");
        auto r = g_app.builder.run(opts,
            [](const hs::BuildProgress& p) {
                json e;
                e["type"] = "build.progress";
                e["percent"] = p.percent;
                e["stage"] = SanitizeUtf8(p.stage);
                PushEventThreadSafe(e.dump());
            },
            []() { return g_buildCancel.load(); });
        r.message = SanitizeUtf8(r.message);   // 净化 GBK 残留 (子进程输出系统码页)
        g_building = false;
        json e;
        e["type"] = "build.done";
        e["ok"] = r.ok;
        e["cancelled"] = r.cancelled;
        e["message"] = r.message;
        e["runtimeVersion"] = r.runtimeVersion;
        e["webUiVersion"] = r.webUiVersion;
        PushEventThreadSafe(e.dump());
        logSink().Log(r.ok ? LogLevel::Info : LogLevel::Error,
                       r.ok ? std::wstring(L"构建完成") : L"构建失败: " + Utf8ToWide(r.message));

        // 零依赖启动闭环: 构建成功 -> 热初始化核心层 -> 通知页面解锁
        if (r.ok && g_app.rootState == RootState::PendingBuild) {
            std::wstring err;
            if (g_app.InitCore(err)) {
                json re;
                re["type"] = "app.rootReady";
                re["root"] = WideToUtf8(g_app.root);
                PushEventThreadSafe(re.dump());
                logSink().Log(LogLevel::Info, L"便携包就绪, 全部功能已解锁");
            } else {
                logSink().Log(LogLevel::Error, L"构建后初始化失败: " + err);
            }
        }
    });
}

// ---- DeepSeek Harness (ADR-008) ----
// 注意: 全部绕过 NeedCore —— DSH 与 Hermes 便携包互相独立, PendingBuild 下可用。
void HDshStart() {
    if (g_busy.exchange(true)) return;
    TaskRunner::Instance().Post([]() {
        std::wstring err;
        bool ok = g_app.dshProc.Start(err);
        g_busy = false;
        logSink().Log(ok ? LogLevel::Info : LogLevel::Error,
                      ok ? L"[dsh] 已请求启动 Web UI" : L"[dsh] 启动失败: " + err);
        PushOp("dsh.start", ok, ok ? L"DSH 启动中, 就绪后可用「打开」进入" : err);
        App_RefreshStatusUI();
    });
}

void HDshStop() {
    if (g_busy.exchange(true)) return;
    TaskRunner::Instance().Post([]() {
        g_app.dshProc.Stop();
        g_busy = false;
        logSink().Log(LogLevel::Info, L"[dsh] 已停止");
        PushOp("dsh.stop", true, L"已停止");
        App_RefreshStatusUI();
    });
}

void HDshOpen() {
    if (!g_app.dshProc.IsUp()) { PushOp("dsh.openBrowser", false, L"DSH 未运行, 请先启动"); return; }
    g_app.dshProc.OpenBrowser();
    PushOp("dsh.openBrowser", true, std::wstring(L"已在系统浏览器打开 (") +
           (g_app.dshProc.TokenUrl().empty() ? L"未捕获 token, 可能需要登录" : L"已带 token") + L")");
}

void HDshRestart() {
    if (g_busy.exchange(true)) return;
    TaskRunner::Instance().Post([]() {
        g_app.dshProc.Stop();
        for (int i = 0; i < 20 && g_app.dshProc.IsUp(); ++i) Sleep(200);
        std::wstring err;
        bool ok = g_app.dshProc.Start(err);
        g_busy = false;
        PushOp("dsh.restart", ok, ok ? L"DSH 重启中, 就绪后页签自动进入" : err);
        logSink().Log(ok ? LogLevel::Info : LogLevel::Error,
                      ok ? L"[dsh] 重启中" : L"[dsh] 重启失败: " + err);
        App_RefreshStatusUI();
    });
}

// ---- DSH 插件 (profiles/web, dsh plugin 转发内置 pnpm; ADR-008) ----
void PushDshPlugins() {
    json e;
    e["type"] = "dshplugin.list";
    e["installed"] = hs::dsh::installed();
    e["plugins"] = json::array();
    for (auto& p : hs::dsh::pluginList())
        e["plugins"].push_back({ {"name", p.name}, {"range", p.range} });
    PushEventThreadSafe(e.dump());
}

void HDshPluginAdd(const json& args) {
    std::string pkg = args.value("pkg", "");
    if (pkg.empty() || pkg.find_first_of(" \t\"'") != std::string::npos) {
        PushOp("dshplugin.add", false, L"包名不能为空或含空格/引号"); return;
    }
    if (!hs::dsh::installed()) { PushOp("dshplugin.add", false, L"DSH 未安装"); return; }
    if (!g_app.dshProc.IsUp()) { PushOp("dshplugin.add", false, L"请先启动 DSH 再安装插件"); return; }
    if (g_busy.exchange(true)) { PushOp("dshplugin.add", false, L"已有任务进行中"); return; }
    TaskRunner::Instance().Post([pkg]() {
        logSink().Log(LogLevel::Info, Utf8ToWide("[dsh] 安装插件: " + pkg));
        auto r = hs::dsh::pluginAdd(pkg);
        g_busy = false;
        PushOp("dshplugin.add", r.ok,
               r.ok ? Utf8ToWide("插件已安装: " + pkg + " (建议重启 DSH 生效)")
                    : Utf8ToWide("安装失败: " + r.output));
        logSink().Log(r.ok ? LogLevel::Info : LogLevel::Error,
                      Utf8ToWide("[dsh] 插件安装 " + pkg + (r.ok ? " 完成" : " 失败: " + r.output)));
        PushDshPlugins();
    });
}

void HDshPluginRemove(const json& args) {
    std::string pkg = args.value("pkg", "");
    if (pkg.empty()) { PushOp("dshplugin.remove", false, L"缺少包名"); return; }
    if (!hs::dsh::installed()) { PushOp("dshplugin.remove", false, L"DSH 未安装"); return; }
    if (g_busy.exchange(true)) { PushOp("dshplugin.remove", false, L"已有任务进行中"); return; }
    TaskRunner::Instance().Post([pkg]() {
        logSink().Log(LogLevel::Info, Utf8ToWide("[dsh] 卸载插件: " + pkg));
        auto r = hs::dsh::pluginRemove(pkg);
        g_busy = false;
        PushOp("dshplugin.remove", r.ok,
               r.ok ? Utf8ToWide("已卸载: " + pkg + " (建议重启 DSH 生效)")
                    : Utf8ToWide("卸载失败: " + r.output));
        logSink().Log(r.ok ? LogLevel::Info : LogLevel::Error,
                      Utf8ToWide("[dsh] 插件卸载 " + pkg + (r.ok ? " 完成" : " 失败")));
        PushDshPlugins();
    });
}

void HDshBuildResolve() {
    TaskRunner::Instance().Post([]() {
        auto info = dsh::resolveLatest();
        json e;
        e["type"] = "dshbuild.remote";
        e["ok"] = info.ok;
        e["message"] = info.message;
        e["dshLatest"] = info.dshLatest;
        e["dshVersions"] = info.dshVersions;
        e["nodeLts"] = info.nodeLts;
        PushEventThreadSafe(e.dump());
        logSink().Log(LogLevel::Info, Utf8ToWide("[dsh] 版本源解析: " + info.message));
    });
}

void HDshBuildStart(const json& args) {
    if (g_dshBuilding.exchange(true)) { PushOp("dsh.build", false, L"DSH 构建已在进行中"); return; }
    TaskRunner::Instance().Post([args]() {
        dsh::BuildOptions opts;
        opts.dshVersion  = args.value("dshVersion", g_app.settings.dsh.pinnedVersion);
        opts.nodeVersion = args.value("nodeVersion", g_app.settings.dsh.nodeVersion);
        if (args.contains("useCache")) opts.useCache = args["useCache"].get<bool>();
        // 目标固定相对便携包根 (与 Hermes 构建的相对路径语义一致)
        opts.targetDir = fs::path(g_app.exeDirW) / L"DSH";
        g_dshBuildCancel = false;
        logSink().Log(LogLevel::Info, L"[dsh] 开始构建 DSH 便携包 (node + dsh)...");
        auto r = dsh::run(opts,
            [](const dsh::BuildProgress& p) {
                json e;
                e["type"] = "dshbuild.progress";
                e["percent"] = p.percent;
                e["stage"] = SanitizeUtf8(p.stage);
                PushEventThreadSafe(e.dump());
            },
            []() { return g_dshBuildCancel.load(); });
        r.message = SanitizeUtf8(r.message);
        g_dshBuilding = false;
        json e;
        e["type"] = "dshbuild.done";
        e["ok"] = r.ok;
        e["cancelled"] = r.cancelled;
        e["message"] = r.message;
        PushEventThreadSafe(e.dump());
        logSink().Log(r.ok ? LogLevel::Info : LogLevel::Error,
                       r.ok ? L"[dsh] 构建完成" : L"[dsh] 构建失败: " + Utf8ToWide(r.message));
    });
}

// ---- 更新 ★ ----
json ComponentJson(const hs::ComponentState& s) {
    auto def = hs::Updater::defOf(s.id);
    json c;
    c["id"] = hs::toString(s.id);
    c["name"] = s.name;
    c["current"] = s.currentVersion;
    c["latest"] = s.latestVersion;
    c["updateAvailable"] = s.updateAvailable;
    c["supported"] = s.supported;
    c["note"] = s.note;
    c["highRisk"] = def.has_value() && def->highRisk;
    return c;
}

// DSH 远端版本缓存 (scanRemote 时刷新, PushUpdateList/apply 读取)
hs::dsh::RemoteInfo& DshRemote() {
    static hs::dsh::RemoteInfo c;
    return c;
}

void PushUpdateList(const std::vector<hs::ComponentState>& states, const std::string& channel,
                    bool withRemote) {
    json e;
    e["type"] = "update.list";
    e["channel"] = channel;
    e["withRemote"] = withRemote;
    e["components"] = json::array();
    for (auto& s : states) e["components"].push_back(ComponentJson(s));
    // DSH 组件 (ADR-008): 与 Hermes 组件同表展示, id = "dsh" / "dshnode"
    if (hs::dsh::installed()) {
        auto v = hs::dsh::queryVersions();
        json d1;
        d1["id"] = "dsh";
        d1["name"] = "DeepSeek Harness";
        d1["current"] = v.dsh;
        d1["latest"] = withRemote ? DshRemote().dshLatest : std::string("-");
        d1["updateAvailable"] = withRemote && !DshRemote().dshLatest.empty()
                                && v.dsh != DshRemote().dshLatest;
        d1["supported"] = true;
        d1["note"] = "数据在 DSH\\dsh, 更新前自动停 DSH 并备份";
        d1["highRisk"] = false;
        e["components"].push_back(d1);
        json d2;
        d2["id"] = "dshnode";
        d2["name"] = "DSH Node.js 运行时";
        d2["current"] = v.node;
        d2["latest"] = withRemote ? DshRemote().nodeLts : std::string("-");
        d2["updateAvailable"] = withRemote && !DshRemote().nodeLts.empty()
                                && v.node != DshRemote().nodeLts && ("v" + v.node) != DshRemote().nodeLts;
        d2["supported"] = true;
        d2["note"] = "DSH 包内 Node, 更新前自动停 DSH 并备份";
        d2["highRisk"] = true;
        e["components"].push_back(d2);
    }
    PushEventThreadSafe(e.dump());
}

void HUpdateScan(bool withRemote, const std::string& channel) {
    // 放宽 NeedCore: DSH 组件独立于 Hermes 核心, PendingBuild 下也要能看/更新 DSH 行
    if (g_busy.exchange(true)) return;
    TaskRunner::Instance().Post([withRemote, channel]() {
        if (withRemote) DshRemote() = hs::dsh::resolveLatest();   // 刷新 DSH 远端版本
        auto states = withRemote ? g_app.updater.scanWithRemote(channel)
                                 : g_app.updater.scanLocal();
        g_busy = false;
        PushUpdateList(states, channel, withRemote);
        logSink().Log(LogLevel::Info, withRemote ? L"远端组件扫描完成" : L"本地组件扫描完成");
    });
}

void HUpdateApply(const json& args) {
    std::string idStr = args.value("componentId", "");
    bool dshComponent = (idStr == "dsh" || idStr == "dshnode");
    if (!dshComponent && !NeedCore("update.apply")) return;
    if (g_busy.exchange(true)) {
        if (!g_busy.load()) PushOp("update.apply", false, L"已有任务进行中");
        return;
    }
    std::string verArg = args.value("version", "");
    hs::ComponentId id;
    bool isDshCore = false, isDshNode = false;
    // 核心层 wire format 是 toString() 的小写短名 (kernel/webui/node); 大驼峰为兼容保留
    if (idStr == "Kernel" || idStr == "kernel")           id = hs::ComponentId::Kernel;
    else if (idStr == "WebUI" || idStr == "webui")        id = hs::ComponentId::WebUI;
    else if (idStr == "NodeRuntime" || idStr == "node")   id = hs::ComponentId::NodeRuntime;
    else if (idStr == "Launcher" || idStr == "launcher")  id = hs::ComponentId::Launcher;
    else if (idStr == "dsh")                              isDshCore = true;
    else if (idStr == "dshnode")                          isDshNode = true;
    else {
        g_busy = false;
        PushOp("update.apply", false, L"该组件暂不支持一键更新 (Python 请走构建重建)");
        return;
    }
    std::string channel = args.value("channel", g_app.settings.runtime.updateChannel);
    TaskRunner::Instance().Post([id, channel, verArg, isDshCore, isDshNode]() {
        auto prog = [](int pct, const std::string& stage) {
            json e;
            e["type"] = "update.progress";
            e["percent"] = pct;
            e["stage"] = stage;
            PushEventThreadSafe(e.dump());
        };
        // 更新前先停 Hermes / DSH (Qt6 行为; DSH 组件更新必须停 dsh)。
        // 停止+备份可能耗时数十秒, 先把阶段透出避免界面停在"准备更新"
        if ((isDshCore || isDshNode)) prog(0, "正在停止相关进程并备份 ...");
        // Launcher 自更新不停 Hermes/DSH: 换的是启动器 exe, 与已拉起的进程无关
        if (id != hs::ComponentId::Launcher && g_app.proc.IsUp()) {
            logSink().Log(LogLevel::Info, L"更新前正在停止 Hermes...");
            g_app.proc.Stop();
        }
        if ((isDshCore || isDshNode) && g_app.dshProc.IsUp()) {
            logSink().Log(LogLevel::Info, L"更新前正在停止 DSH...");
            g_app.dshProc.Stop();
        }
        auto dprog = [&prog](const hs::dsh::BuildProgress& p) { prog(p.percent, p.stage); };
        hs::Updater::ApplyResult r;
        if (isDshCore) {
            std::string dtarget = verArg.empty() ? DshRemote().dshLatest : verArg;
            if (dtarget.empty()) {
                g_busy = false;
                PushOp("update.apply", false, L"无法确定远端版本 (先「检查远端」)");
                return;
            }
            auto dr = hs::dsh::updateCore(dtarget, dprog);
            r.ok = dr.ok; r.message = dr.message;
        } else if (isDshNode) {
            std::string dtarget = verArg.empty() ? DshRemote().nodeLts : verArg;
            if (dtarget.empty()) {
                g_busy = false;
                PushOp("update.apply", false, L"无法确定远端版本 (先「检查远端」)");
                return;
            }
            auto dr = hs::dsh::updateNode(dtarget, dprog);
            r.ok = dr.ok; r.message = dr.message;
        } else {
            std::string target = verArg;
            if (target.empty()) {
                auto rr = g_app.updater.checkRemote(id, channel);
                if (rr.latestVersion.empty()) {
                    g_busy = false;
                    PushOp("update.apply", false, Utf8ToWide("无法确定远端版本: " + rr.message));
                    return;
                }
                target = rr.latestVersion;
            }
            if (id == hs::ComponentId::Kernel)      r = g_app.updater.applyKernelUpdate(target, prog);
            else if (id == hs::ComponentId::WebUI)  r = g_app.updater.applyWebUiUpdate(target, prog);
            else if (id == hs::ComponentId::Launcher) r = g_app.updater.applyLauncherUpdate(target, prog);
            else                                    r = g_app.updater.applyNodeUpdate(target, prog);
        }
        g_busy = false;
        PushOp("update.apply", r.ok, Utf8ToWide(r.message));
        logSink().Log(r.ok ? LogLevel::Info : LogLevel::Error, Utf8ToWide(r.message));
        // 更新后自动重扫 (Qt6 行为)
        auto states = g_app.updater.scanWithRemote(channel);
        PushUpdateList(states, channel, true);
    });
}

void HUpdateNodeVersions() {
    if (!NeedCore("update.nodeVersions")) return;
    TaskRunner::Instance().Post([]() {
        auto vers = g_app.updater.listNodeVersions(30);
        json e;
        e["type"] = "update.nodeList";
        e["versions"] = vers;
        PushEventThreadSafe(e.dump());
    });
}

void HUpdateBackups() {
    if (!NeedCore("update.backups")) return;
    TaskRunner::Instance().Post([]() {
        // 枚举 studio/backups/<组件>/<tag>/ 全量 (不猜组件名)
        json e;
        e["type"] = "update.backupList";
        e["backups"] = json::array();
        std::error_code ec;
        fs::path root = hs::Paths::backupDir();
        for (fs::directory_iterator it(root, ec), end; it != end; ++it) {
            if (!it->is_directory(ec)) continue;
            std::string comp = WideToUtf8(it->path().filename().wstring());
            for (fs::directory_iterator ti(it->path(), ec), tend; ti != tend; ++ti) {
                if (!ti->is_directory(ec)) continue;
                e["backups"].push_back({
                    {"component", comp},
                    {"tag", WideToUtf8(ti->path().filename().wstring())},
                    {"path", WideToUtf8(ti->path().wstring())} });
            }
        }
        PushEventThreadSafe(e.dump());
    });
}

void HUpdateRollback(const json& args) {
    if (!NeedCore("update.rollback")) return;
    std::string path = args.value("path", "");
    if (path.empty()) { PushOp("update.rollback", false, L"缺少备份路径"); return; }
    TaskRunner::Instance().Post([path]() {
        hs::UpdateManager um;
        std::string err;
        bool ok = um.rollback(Utf8ToWide(path), &err);
        PushOp("update.rollback", ok, ok ? L"已回滚, 建议重启网关" : Utf8ToWide(err.empty() ? "回滚失败" : err));
        logSink().Log(ok ? LogLevel::Info : LogLevel::Error,
                       ok ? L"已回滚备份: " + Utf8ToWide(path) : L"回滚失败: " + Utf8ToWide(err));
    });
}

void HUpdateBackupDelete(const json& args) {
    if (!NeedCore("update.backupDelete")) return;
    std::string path = args.value("path", "");
    if (path.empty()) { PushOp("update.backupDelete", false, L"缺少备份路径"); return; }
    TaskRunner::Instance().Post([path]() {
        hs::UpdateManager um;
        std::string err;
        bool ok = um.removeBackup(Utf8ToWide(path), &err);
        PushOp("update.backupDelete", ok, ok ? L"备份已删除" : Utf8ToWide(err.empty() ? "删除失败" : err));
        logSink().Log(ok ? LogLevel::Info : LogLevel::Error,
                       ok ? L"已删除备份: " + Utf8ToWide(path) : L"删除备份失败: " + Utf8ToWide(err));
    });
}

// ---- 体检 ----
void HPreflightRun() {
    // 放宽 NeedCore: DSH 检查项独立于 Hermes 核心, PendingBuild 下同样可体检
    TaskRunner::Instance().Post([]() {
        hs::PreflightChecker::Context ctx;
        ctx.modelConfig = g_app.registry ? g_app.registry->config() : hs::ModelConfig{};
        ctx.runtime = g_app.settings.runtime;
        ctx.haveModels = hs::JsonStore::load(hs::Paths::studioDir() / "models.json").has_value();
        ctx.hermesRunning = g_app.proc.IsUp();
        hs::PreflightChecker checker(ctx);
        auto items = checker.runAll();
        // DSH 检查 (ADR-008): 不阻断 Hermes 启动, 缺失记 WARN
        {
            hs::CheckItem it1;
            it1.id = "dsh_install"; it1.title = "DSH 便携包安装";
            if (hs::dsh::installed()) {
                it1.level = hs::CheckLevel::Pass; it1.detail = ".\\DSH 布局就绪 (node + dsh)";
            } else {
                it1.level = hs::CheckLevel::Warn; it1.detail = "未检测到 .\\DSH, 可在「构建」页一键构建";
            }
            items.push_back(it1);
            if (hs::dsh::installed()) {
                hs::CheckItem it2;
                it2.id = "dsh_versions"; it2.title = "DSH 组件版本";
                auto v = hs::dsh::queryVersions();
                if (v.error.empty()) {
                    it2.level = hs::CheckLevel::Pass;
                    it2.detail = "node " + v.node + " / dsh " + v.dsh;
                } else {
                    it2.level = hs::CheckLevel::Warn; it2.detail = v.error;
                }
                items.push_back(it2);
            }
            hs::CheckItem it3;
            it3.id = "dsh_port"; it3.title = "DSH 端口";
            int dport = g_app.settings.dsh.webUiPort;
            auto occ = net::pidByPort(dport);
            if (occ && g_app.dshProc.IsUp()) {
                it3.level = hs::CheckLevel::Pass;
                it3.detail = "127.0.0.1:" + std::to_string(dport) + " 监听中 (DSH 运行, pid " + std::to_string(*occ) + ")";
            } else if (occ) {
                it3.level = hs::CheckLevel::Warn;
                it3.detail = "端口 " + std::to_string(dport) + " 被其他进程占用 (pid " + std::to_string(*occ) + "), DSH 无法启动";
            } else {
                it3.level = hs::CheckLevel::Pass;
                it3.detail = "端口 " + std::to_string(dport) + " 空闲";
            }
            items.push_back(it3);
            // pnpm: Git 仓库形插件的依赖解析需要 (缺失可一键自动装入包内)
            hs::CheckItem it4;
            it4.id = "dsh_pnpm"; it4.title = "DSH 插件 pnpm";
            auto pv = hs::dsh::pnpmVersion();
            if (!pv.empty()) {
                it4.level = hs::CheckLevel::Pass;
                it4.detail = "pnpm " + pv + " 就绪";
            } else {
                it4.level = hs::CheckLevel::Warn;
                it4.detail = "未检测到 pnpm — 安装 Git 仓库形插件 (github:/git clone 链接) 时会自动装入 DSH\\node, 也可点右侧立即安装";
                it4.fixable = true;
                it4.fixAction = "auto:install_pnpm";
            }
            items.push_back(it4);
            // git: 仅私有仓库 / pnpm 无法走 tarball 的来源才必需
            hs::CheckItem it5;
            it5.id = "dsh_git"; it5.title = "DSH 插件 git";
            if (hs::dsh::gitAvailable()) {
                it5.level = hs::CheckLevel::Pass;
                it5.detail = "git 可用 (公开仓库走 tarball 拉取, 通常不需要)";
            } else {
                it5.level = hs::CheckLevel::Warn;
                it5.detail = "未检测到 git — 公开仓库插件不受影响; 私有仓库插件需要自行安装 git";
            }
            items.push_back(it5);
        }
        int pass = 0, warn = 0, fail = 0;
        json arr = json::array();
        for (auto& c : items) {
            json it;
            it["id"] = c.id;
            it["title"] = c.title;
            it["level"] = c.level == hs::CheckLevel::Pass ? "pass"
                        : c.level == hs::CheckLevel::Warn ? "warn" : "fail";
            it["detail"] = c.detail;
            it["fixable"] = c.fixable;
            it["fixAction"] = c.fixAction;
            arr.push_back(it);
            if (c.level == hs::CheckLevel::Pass) ++pass;
            else if (c.level == hs::CheckLevel::Warn) ++warn;
            else ++fail;
        }
        json e;
        e["type"] = "preflight.result";
        e["items"] = arr;
        e["summary"] = { {"pass", pass}, {"warn", warn}, {"fail", fail},
                         {"canLaunch", fail == 0} };
        PushEventThreadSafe(e.dump());
    });
}

void HPreflightFix(const json& args) {
    // 放宽 NeedCore: auto:install_pnpm 等 DSH 修复独立于 Hermes 核心
    std::string action = args.value("fixAction", "");
    TaskRunner::Instance().Post([action]() {
        // DSH 自装: pnpm (Git 仓库形插件依赖, ADR-008)
        auto execFix = [&](const std::string& act) -> std::string {
            if (act == "auto:install_pnpm") {
                auto r = hs::dsh::installPnpm();
                std::string msg = r.ok
                    ? "pnpm " + r.output + " 已自动装入 DSH\\node"
                    : "pnpm 自动安装失败: " + r.output;
                logSink().Log(r.ok ? LogLevel::Info : LogLevel::Error, Utf8ToWide("体检修复: " + msg));
                return msg;
            }
            hs::PreflightChecker::Context ctx;
            ctx.runtime = g_app.settings.runtime;
            hs::PreflightChecker checker(ctx);
            std::string msg = checker.applyFix(act);
            logSink().Log(LogLevel::Info, Utf8ToWide("体检修复: " + msg));
            return msg;
        };
        if (action == "__fixall__") {
            // 批量执行全部 auto: 项 (Qt6「修复可修复项」行为)
            hs::PreflightChecker::Context ctx;
            ctx.modelConfig = g_app.registry ? g_app.registry->config() : hs::ModelConfig{};
            ctx.runtime = g_app.settings.runtime;
            ctx.haveModels = hs::JsonStore::load(hs::Paths::studioDir() / "models.json").has_value();
            ctx.hermesRunning = g_app.proc.IsUp();
            hs::PreflightChecker checker(ctx);
            int n = 0;
            for (auto& c : checker.runAll())
                if (c.fixable && c.fixAction.rfind("auto:", 0) == 0) { execFix(c.fixAction); ++n; }
            PushOp("preflight.fixAll", true, Utf8ToWide("已执行 " + std::to_string(n) + " 项自动修复"));
        } else {
            std::string msg = execFix(action);
            bool ok = msg.find("失败") == std::string::npos;
            PushOp("preflight.fix", ok, Utf8ToWide(msg.empty() ? "已执行修复" : msg));
        }
        HPreflightRun();   // 修复后重查
    });
}

// ---- Agent 注册表 (ADR-009): 构建页/页签/更新中心按描述符取共享元数据 ----
void HAgentList() {
    json arr = json::array();
    for (auto& a : hs::agents::registry()) {
        arr.push_back({{"id", a.id}, {"displayName", a.displayName},
                       {"buildTitle", a.buildTitle}, {"buildNote", a.buildNote},
                       {"targetDirLabel", a.targetDirLabel}, {"defaultPort", a.defaultPort},
                       {"updateIds", a.updateIds}, {"hasBuilder", a.hasBuilder}});
    }
    json e;
    e["type"] = "agent.list";
    e["agents"] = arr;
    PushEventThreadSafe(e.dump());
}

// ---- Provider / 模型 ----
// 预设表只读静态数据, 不依赖核心层初始化
void HProviderPresets() {
    json arr = json::array();
    for (const auto& pr : hs::providerPresets()) {
        json models = json::array();
        for (const auto& m : pr.models)
            models.push_back({{"upstreamId", m.upstreamId}, {"displayName", m.displayName}});
        arr.push_back({{"id", pr.id}, {"displayName", pr.displayName}, {"group", pr.group},
                       {"kind", pr.kind}, {"baseUrl", pr.baseUrl},
                       {"baseUrlEnvVar", pr.baseUrlEnvVar}, {"providerName", pr.providerName},
                       {"envVar", pr.envVar}, {"keyRequired", pr.keyRequired},
                       {"keyUrl", pr.keyUrl}, {"notes", pr.notes}, {"models", models}});
    }
    json e;
    e["type"] = "provider.presets";
    e["presets"] = arr;
    PushEventThreadSafe(e.dump());
}

void PushProviders() {
    json e;
    e["type"] = "provider.list";
    e["providers"] = json::array();
    if (g_app.registry) {
        for (auto& p : g_app.registry->providers()) {
            json j = p;   // 手写 to_json (Types.h), 含 providerName/baseUrlEnvVar
            e["providers"].push_back(j);
        }
    }
    PushEventThreadSafe(e.dump());
}

void PushModels() {
    json e;
    e["type"] = "model.list";
    e["models"] = json::array();
    if (g_app.registry) {
        for (auto& m : g_app.registry->models()) {
            json j = m;
            e["models"].push_back(j);
        }
    }
    PushEventThreadSafe(e.dump());
}

// ---- 插件 ----
void PushPlugins() {
    json e;
    e["type"] = "plugin.list";
    e["plugins"] = json::array();
    e["switches"] = json::object();          // HSPP v1.3 switch 动作的宿主侧状态快照
    for (auto& [pid, sw] : g_app.plugins.allSwitchStates())
        for (auto& [aid, v] : sw) e["switches"][pid][aid] = v;
    int sidebarCount = 0;
    for (auto& p : g_app.plugins.allPlugins()) {
        json j;
        j["id"] = p.id;
        j["name"] = p.name;
        j["version"] = p.version;
        j["author"] = p.author;
        j["description"] = p.description;
        j["enabled"] = p.enabled;
        j["loaded"] = p.loaded;
        j["loadError"] = p.loadError;
        j["sidebarEnabled"] = p.sidebarEnabled;
        j["quick"] = p.quick;                // HSPP v1.3: 是否进快捷区
        j["uiEntries"] = json::array();
        for (auto& u : p.uiEntries) {
            if (p.loaded && u.sidebar) ++sidebarCount;
            j["uiEntries"].push_back({ {"id", u.id}, {"title", u.title}, {"icon", u.icon},
                                       {"jsonSchema", u.jsonSchema}, {"defaultConfig", u.defaultConfig},
                                       {"actions", u.actions}, {"sidebar", u.sidebar} });
        }
        e["plugins"].push_back(j);
    }
    logSink().Log(LogLevel::Info,
        Utf8ToWide("[plugin] list 推送: 插件 " + std::to_string((int)g_app.plugins.allPlugins().size())
                   + " 个, 已加载 " + std::to_string((int)g_app.plugins.loadedPlugins().size())
                   + " 个, 侧边栏入口 " + std::to_string(sidebarCount) + " 个"));
    PushEventThreadSafe(e.dump());
}

// ---- Provider / 模型 CRUD ----
void HProviderSave(const json& args, bool isUpdate) {
    if (!NeedCore("provider.save") || !g_app.registry) return;
    hs::Provider p;
    try { p = args.get<hs::Provider>(); } catch (...) {
        PushOp("provider.save", false, L"参数格式错误"); return;
    }
    if (p.envVar.empty() && !p.id.empty()) {
        std::string up = p.id;
        for (auto& c : up) c = (char)toupper((unsigned char)c);
        p.envVar = up + "_API_KEY";
    }
    // 编辑时仅当 Key 输入框留空才保留旧值 (否则表单里新粘贴的 Key 会被旧值覆盖)
    if (isUpdate && p.apiKey.empty() && g_app.registry->provider(p.id))
        p.apiKey = g_app.registry->provider(p.id)->apiKey;
    auto r = isUpdate ? g_app.registry->updateProvider(p) : g_app.registry->addProvider(p);
    if (!r.ok()) { PushOp("provider.save", false, Utf8ToWide(r.error->message)); return; }
    if (g_app.renderer)
        (void)g_app.renderer->renderAll(g_app.registry->config(), g_app.settings.runtime, {});
    PushOp("provider.save", true, Utf8ToWide("Provider 已保存: " + p.id));
    PushProviders();
}

void HProviderRemove(const json& args) {
    if (!NeedCore("provider.remove") || !g_app.registry) return;
    std::string id = args.value("id", "");
    auto r = g_app.registry->removeProvider(id);
    if (!r.ok()) { PushOp("provider.remove", false, Utf8ToWide(r.error->message)); return; }
    if (g_app.renderer)
        (void)g_app.renderer->renderAll(g_app.registry->config(), g_app.settings.runtime, {});
    PushOp("provider.remove", true, Utf8ToWide("已删除 Provider: " + id));
    PushProviders();
    PushModels();
}

void HProviderToggle(const json& args) {
    if (!NeedCore("provider.toggle") || !g_app.registry) return;
    std::string id = args.value("id", "");
    auto p = g_app.registry->provider(id);
    if (!p) { PushOp("provider.toggle", false, L"未找到该 Provider"); return; }
    p->enabled = !p->enabled;
    auto r = g_app.registry->updateProvider(*p);
    if (!r.ok()) { PushOp("provider.toggle", false, Utf8ToWide(r.error->message)); return; }
    if (g_app.renderer)
        (void)g_app.renderer->renderAll(g_app.registry->config(), g_app.settings.runtime, {});
    PushOp("provider.toggle", true, Utf8ToWide(id + (p->enabled ? " 已启用" : " 已停用")));
    PushProviders();
}

void HModelSave(const json& args, bool isUpdate) {
    if (!NeedCore("model.save") || !g_app.registry) return;
    hs::Model m;
    try { m = args.get<hs::Model>(); } catch (...) {
        PushOp("model.save", false, L"参数格式错误"); return;
    }
    if (m.upstreamId.empty()) m.upstreamId = m.id;
    if (m.displayName.empty()) m.displayName = m.id;
    auto r = isUpdate ? g_app.registry->updateModel(m) : g_app.registry->addModel(m);
    if (!r.ok()) { PushOp("model.save", false, Utf8ToWide(r.error->message)); return; }
    if (g_app.renderer)
        (void)g_app.renderer->renderAll(g_app.registry->config(), g_app.settings.runtime, {});
    PushOp("model.save", true, Utf8ToWide("模型已保存: " + m.id));
    PushModels();
}

void HModelRemove(const json& args) {
    if (!NeedCore("model.remove") || !g_app.registry) return;
    std::string id = args.value("id", "");
    auto r = g_app.registry->removeModel(id);
    if (!r.ok()) { PushOp("model.remove", false, Utf8ToWide(r.error->message)); return; }
    if (g_app.renderer)
        (void)g_app.renderer->renderAll(g_app.registry->config(), g_app.settings.runtime, {});
    PushOp("model.remove", true, Utf8ToWide("已删除模型: " + id));
    PushModels();
}

void HModelDefault(const json& args) {
    if (!NeedCore("model.default") || !g_app.registry) return;
    std::string id = args.value("id", "");
    auto r = g_app.registry->setDefault(id);
    if (!r.ok()) { PushOp("model.default", false, Utf8ToWide(r.error->message)); return; }
    if (g_app.renderer)
        (void)g_app.renderer->renderAll(g_app.registry->config(), g_app.settings.runtime, {});
    PushOp("model.default", true, Utf8ToWide("已设为默认模型: " + id));
    PushModels();
}

// ---- 插件 ----
void HPluginToggle(const json& args) {
    if (!NeedCore("plugin.toggle")) return;
    std::string id = args.value("id", "");
    std::string err;
    bool target = !args.value("enabled", true);
    if (!g_app.plugins.setEnabled(id, target, &err)) {
        PushOp("plugin.toggle", false, Utf8ToWide(err));
        return;
    }
    g_app.plugins.reload();
    PushOp("plugin.toggle", true, Utf8ToWide(id + (target ? " 已启用" : " 已停用")));
    PushPlugins();
}

void HPluginSidebar(const json& args) {
    if (!NeedCore("plugin.sidebar")) return;
    std::string id = args.value("id", "");
    bool visible = args.value("visible", true);
    std::string err;
    if (!g_app.plugins.setSidebar(id, visible, &err)) {
        PushOp("plugin.sidebar", false, Utf8ToWide(err));
        return;
    }
    PushOp("plugin.sidebar", true,
           Utf8ToWide(id + (visible ? " 已显示在侧边栏" : " 已从侧边栏隐藏")));
    PushPlugins();
}

void HPluginRemove(const json& args) {
    if (!NeedCore("plugin.remove")) return;
    std::string id = args.value("id", "");
    std::string err;
    if (!g_app.plugins.removePlugin(id, &err)) {
        PushOp("plugin.remove", false, Utf8ToWide(err));
        return;
    }
    g_app.plugins.reload();
    PushOp("plugin.remove", true, Utf8ToWide("已删除插件: " + id));
    PushPlugins();
}

void HPluginAction(const json& args) {
    if (!NeedCore("plugin.action")) return;
    std::string pid = args.value("pluginId", "");
    std::string aid = args.value("actionId", "");
    // HSPP v1.3: payload 由调用方携带 (开关 {"value":bool}; 带 jsonSchema 的表单动作传对象)
    std::string payload = "{}";
    if (args.contains("payload")) {
        try { payload = args.at("payload").dump(); } catch (...) {}
    }
    TaskRunner::Instance().Post([pid, aid, payload]() {
        auto out = g_app.plugins.executeActionEx(pid, aid, payload);
        bool ok = (out.rc == 0);
        std::wstring msg;
        if (!ok) {
            msg = Utf8ToWide("插件动作失败 (rc=" + std::to_string(out.rc) + ")");
            if (!out.logTail.empty()) msg += L"\n" + Utf8ToWide(out.logTail);
        } else if (!out.logTail.empty()) {
            msg = Utf8ToWide(out.logTail);          // 插件执行明细 (清理统计等)
        } else {
            msg = L"插件动作完成";
        }
        PushOp("plugin.action", ok, msg);
        json e;
        e["type"] = "plugin.action.result";
        e["pluginId"] = pid; e["actionId"] = aid;
        e["ok"] = ok; e["rc"] = out.rc;
        e["message"] = out.logTail;
        // HSPP v1.4: 插件经 "@@DATA@@{json}" 回传的结构化结果 (如清理扫描清单)
        if (!out.data.empty()) {
            try { e["data"] = json::parse(out.data); } catch (...) {}
        }
        PushEventThreadSafe(e.dump());
        logSink().Log(ok ? LogLevel::Info : LogLevel::Error,
                      Utf8ToWide("插件动作 " + pid + "/" + aid +
                                 (ok ? " 完成" : " 失败 rc=" + std::to_string(out.rc))));
    });
}

// HSPP v1.3 switch 动作: 宿主翻转持久化状态, 以 {"value": 新状态} 调插件, 推回列表同步各载体
void HPluginSwitch(const json& args) {
    if (!NeedCore("plugin.switch")) return;
    std::string pid = args.value("pluginId", "");
    std::string aid = args.value("actionId", "");
    bool value = args.value("value", false);
    bool declaredDefault = args.value("default", false);
    TaskRunner::Instance().Post([pid, aid, value, declaredDefault]() {
        g_app.plugins.setSwitchState(pid, aid, value);
        json payload; payload["value"] = value;
        auto out = g_app.plugins.executeActionEx(pid, aid, payload.dump());
        bool ok = (out.rc == 0);
        json e;
        e["type"] = "plugin.action.result";
        e["pluginId"] = pid; e["actionId"] = aid;
        e["ok"] = ok; e["rc"] = out.rc; e["switch"] = value;
        e["message"] = out.logTail;
        PushEventThreadSafe(e.dump());
        PushPlugins();      // 让悬浮菜单外的所有载体同步开关状态
        logSink().Log(ok ? LogLevel::Info : LogLevel::Error,
                      Utf8ToWide("插件开关 " + pid + "/" + aid + " -> " +
                                 (value ? "ON" : "OFF") + (ok ? "" : " (执行失败)")));
    });
}

// zip 一键安装: 解压 -> 清单校验 -> 落位 -> 自动重扫
void HPluginInstall(const json& args) {
    if (!NeedCore("plugin.install")) return;
    std::string path = args.value("path", "");
    if (path.empty()) { PushOp("plugin.install", false, L"未指定安装包路径"); return; }
    TaskRunner::Instance().Post([path]() {
        std::string err;
        bool ok = g_app.plugins.installFromZip(fs::path(Utf8ToWide(path)), &err);
        PushOp("plugin.install", ok, ok ? L"插件安装完成" : Utf8ToWide(err));
        if (ok) PushPlugins();
    });
}

// 原生文件选择对话框 (WebView 的 <input type=file> 拿不到完整路径)
void HAppPickFile(const json& args) {
    std::string purpose = args.value("purpose", "");
    OPENFILENAMEW ofn{};
    wchar_t buf[MAX_PATH]{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_app.main;
    ofn.lpstrFilter = L"插件包 (*.zip)\0*.zip\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    std::string picked;
    if (GetOpenFileNameW(&ofn)) picked = WideToUtf8(buf);
    json e;
    e["type"] = "app.pickFile.result";
    e["purpose"] = purpose;
    e["path"] = picked;
    PushEventThreadSafe(e.dump());
}

// ---- 设置 ----
void PushSettings() {
    json e;
    e["type"] = "settings.state";
    json s = g_app.settings;
    e["settings"] = s;
    e["rootState"] = g_app.rootState == RootState::Full ? "full" : "pendingBuild";
    e["root"] = WideToUtf8(g_app.root);
    e["coreReady"] = g_app.coreReady;
    PushEventThreadSafe(e.dump());
}

void HSettingsSave(const json& args) {
    try {
        g_app.settings = args.at("settings").get<hs::AppSettings>();
    } catch (...) { PushOp("settings.save", false, L"参数格式错误"); return; }
    g_app.SaveSettings();
    // 端口变化 -> 立即生效 (下次启动用新端口)
    if (g_app.settings.runtime.webUiPort > 0)
        g_app.port = g_app.settings.runtime.webUiPort;
    // DSH 端口变化 -> 重新解析路径/端口 (运行中的 dsh 不受影响, 下次启动生效)
    if (g_app.settings.dsh.webUiPort > 0)
        g_app.dshProc.Init(fs::path(g_app.exeDirW) / L"DSH", g_app.settings.dsh.webUiPort);
    g_app.plugins.dispatch(HS_EVENT_SETTINGS_SAVED);
    PushOp("settings.save", true, L"设置已保存");
    PushSettings();
}

// ---- 网络诊断 (设置页「测试代理」) ----
void HNetTestProxy(const json& args) {
    if (g_busy.exchange(true)) { PushOp("net.testProxy", false, L"已有任务进行中"); return; }
    // 优先用页面传来的表单值 (可能尚未保存), 缺省用 settings
    json net = args.contains("settings") && args["settings"].is_object()
             && args["settings"].contains("network") ? args["settings"]["network"]
                                                     : json(g_app.settings.network);
    TaskRunner::Instance().Post([net]() {
        net::ProxyConfig pc;
        int mode = net.value("mode", 0);
        pc.mode = mode == 1 ? net::ProxyConfig::Mode::Direct
                : mode == 2 ? net::ProxyConfig::Mode::Manual
                : net::ProxyConfig::Mode::System;
        if (pc.mode == net::ProxyConfig::Mode::Manual) {
            pc.host = net.value("host", std::string{});
            pc.port = net.value("port", 0);
        }
        auto r = net::testProxy(pc);
        g_busy = false;
        std::wstring msg = r.ok
            ? L"代理可用 (" + std::to_wstring(r.latencyMs) + L" ms) — " + Utf8ToWide(r.detail)
            : L"代理不可用 — " + Utf8ToWide(r.detail);
        logSink().Log(r.ok ? LogLevel::Info : LogLevel::Error, Utf8ToWide("[proxy] 测试: ") + msg);
        PushOp("net.testProxy", r.ok, msg);
    });
}

// ---- 日志 ----
void HLogQuery(const json& args) {
    std::string level = args.value("level", "all");
    std::string keyword = args.value("keyword", "");
    int limit = args.value("limit", 500);
    auto entries = logSink().Query(level, keyword, limit);
    json e;
    e["type"] = "log.entries";
    e["entries"] = json::array();
    for (auto& en : entries)
        e["entries"].push_back({ {"ts", WideToUtf8(en.ts)},
                                 {"level", WideToUtf8(LogSink::LevelName(en.level))},
                                 {"text", WideToUtf8(en.text)} });
    PushEventThreadSafe(e.dump());
}

void HLogExport() {
    auto entries = logSink().Query("all", "", 100000);
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[80];
    swprintf_s(name, L"launcher-export-%04u%02u%02u-%02u%02u%02u.txt",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    fs::path out = fs::path(g_app.LogsDirW()) / name;
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    if (!f) { PushOp("log.export", false, L"导出失败: 无法写入文件"); return; }
    for (auto& en : entries) {
        f << "[" << WideToUtf8(en.ts) << "] [" << WideToUtf8(LogSink::LevelName(en.level)) << "] "
          << WideToUtf8(en.text) << "\n";
    }
    PushOp("log.export", true, L"已导出: " + out.wstring());
}

}  // namespace

// 自更新后重启: 拉起新 exe 并关闭当前窗口退出。exe 已由 applyLauncherUpdate
// 就位; 若拉起失败 (权限/杀软), 只提示不阻塞 —— 用户手动启动即可。
void HAppRelaunch() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring cmdLine = L"\"" + std::wstring(buf) + L"\"";
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, FALSE, 0,
                       nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        logSink().Log(LogLevel::Info, L"[selfupdate] 已拉起新版启动器, 退出当前实例");
        if (g_app.main) PostMessageW(g_app.main, WM_CLOSE, 0, 0);
    } else {
        PushOp("app.relaunch", false,
               L"重启失败 (错误码 " + std::to_wstring(GetLastError()) + L"), 请手动启动");
    }
}

// 启动时自动检查远端 (main.cpp autoCheckUpdate 调用)。
// 必须与 HUpdateScan 走同一条 PushUpdateList 路径 —— 曾各自拼 update.list,
// 自动检查那条没有 DSH 行, 启动 0.8s 后把页面上的 dsh/dshnode 行覆盖掉。
void Bridge_AutoUpdateScan() {
    TaskRunner::Instance().Post([]() {
        Sleep(800);
        DshRemote() = hs::dsh::resolveLatest();
        auto states = g_app.updater.scanWithRemote(g_app.settings.runtime.updateChannel);
        PushUpdateList(states, g_app.settings.runtime.updateChannel, true);
        logSink().Log(LogLevel::Info, L"启动时自动检查远端完成 (含 DSH 组件)");
    });
}

// 启动器页 NavigationCompleted 后重推插件列表。
// 页面初始化早期的 plugin.list 响应可能落在导航未完成窗口期, ExecuteScript
// 静默失败 (本项目已知坑) —— 页面侧表现为侧边栏入口缺失, 重扫才出现。
void Bridge_OnShellNavigationCompleted() {
    PushPlugins();
}

// ---- 入口: 命令分发 (UI 线程, 由 WebViews WebMessageReceived 调用) ----
void Bridge_HandleJson(const std::string& jsonUtf8) {
    json req;
    try { req = json::parse(jsonUtf8); } catch (...) { return; }
    if (!req.is_object()) return;
    std::string cmd = req.value("cmd", "");
    logSink().Log(LogLevel::Info, Utf8ToWide("[bridge] cmd=" + cmd));
    const json& args = req;

    if (cmd == "proc.start")             HProcStart();
    else if (cmd == "proc.stop")         HProcStop();
    else if (cmd == "proc.restart")      HProcRestart();
    else if (cmd == "proc.openBrowser")  g_app.proc.OpenBrowser();
    else if (cmd == "dsh.start")         HDshStart();
    else if (cmd == "dsh.stop")          HDshStop();
    else if (cmd == "dsh.restart")       HDshRestart();
    else if (cmd == "dsh.openBrowser")   HDshOpen();
    else if (cmd == "dshplugin.list")    PushDshPlugins();
    else if (cmd == "dshplugin.add")     HDshPluginAdd(args);
    else if (cmd == "dshplugin.remove")  HDshPluginRemove(args);
    else if (cmd == "build.resolve")     HBuildResolve();
    else if (cmd == "build.start")       HBuildStart(args);
    else if (cmd == "build.cancel")      g_buildCancel = true;
    else if (cmd == "build.dsh.resolve") HDshBuildResolve();
    else if (cmd == "build.dsh.start")   HDshBuildStart(args);
    else if (cmd == "build.dsh.cancel")  g_dshBuildCancel = true;
    else if (cmd == "update.scanLocal")  HUpdateScan(false, args.value("channel", g_app.settings.runtime.updateChannel));
    else if (cmd == "update.scanRemote") HUpdateScan(true, args.value("channel", g_app.settings.runtime.updateChannel));
    else if (cmd == "update.apply")      HUpdateApply(args);
    else if (cmd == "update.nodeVersions") HUpdateNodeVersions();
    else if (cmd == "update.backups")    HUpdateBackups();
    else if (cmd == "update.backupDelete") HUpdateBackupDelete(args);
    else if (cmd == "update.rollback")   HUpdateRollback(args);
    else if (cmd == "preflight.run")     HPreflightRun();
    else if (cmd == "preflight.fixAll")  HPreflightFix(json{ {"fixAction", "__fixall__"} });
    else if (cmd == "preflight.fix")     HPreflightFix(args);
    else if (cmd == "agent.list")        HAgentList();
    else if (cmd == "provider.presets")  HProviderPresets();
    else if (cmd == "provider.list")     { if (NeedCore("provider.list")) PushProviders(); }
    else if (cmd == "provider.save")     HProviderSave(args, args.value("isUpdate", false));
    else if (cmd == "provider.remove")   HProviderRemove(args);
    else if (cmd == "provider.toggle")   HProviderToggle(args);
    else if (cmd == "model.list")        { if (NeedCore("model.list")) PushModels(); }
    else if (cmd == "model.save")        HModelSave(args, args.value("isUpdate", false));
    else if (cmd == "model.remove")      HModelRemove(args);
    else if (cmd == "model.default")     HModelDefault(args);
    else if (cmd == "plugin.list")       PushPlugins();
    else if (cmd == "plugin.toggle")     HPluginToggle(args);
    else if (cmd == "plugin.sidebar")    HPluginSidebar(args);
    else if (cmd == "plugin.remove")     HPluginRemove(args);
    else if (cmd == "plugin.action")     HPluginAction(args);
    else if (cmd == "plugin.switch")     HPluginSwitch(args);
    else if (cmd == "plugin.install")    HPluginInstall(args);
    else if (cmd == "settings.get")      PushSettings();
    else if (cmd == "settings.save")     HSettingsSave(args);
    else if (cmd == "net.testProxy")     HNetTestProxy(args);
    else if (cmd == "log.query")         HLogQuery(args);
    else if (cmd == "log.export")        HLogExport();
    else if (cmd == "app.diag")         logSink().Log(LogLevel::Info, Utf8ToWide("[page] " + req.value("text", std::string{})));
    else if (cmd == "app.pickFile")     HAppPickFile(args);   // 原生文件选择 (WebView 无 full path 能力)
    else if (cmd == "app.openRoot")      ShellExecuteW(nullptr, L"open", g_app.exeDirW.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    else if (cmd == "app.relaunch")      HAppRelaunch();   // 自更新后就地重启
    else if (cmd == "app.openUrl") {
        // 仅允许 http/https 外链 (系统浏览器打开, WebView 内不跳转)
        std::string url = req.value("url", "");
        if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0)
            ShellExecuteW(nullptr, L"open", Utf8ToWide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    else if (cmd == "app.openDownloads") {
        std::wstring dir = hs::Paths::downloadDir().wstring();
        CreateDirectoryW(dir.c_str(), nullptr);
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    else if (cmd == "fab.openMenu")      FabMenu_ShowAtFab();
    else { /* 未知命令静默丢弃 */ }
}

void Bridge_Init() {
    g_uiThread = GetCurrentThreadId();
}

// 事件出口: 原生汇入始终执行; WebView 端统一入队, 由 30ms 节流 timer 冲刷。
// 实测 (wv2test + 本机): ExecuteScript 仅在消息处理上下文可靠, WM_TIMER /
// 后台 marshal 上下文直调会静默失败 —— 故所有推送统一走 timer 冲刷通道。
static std::vector<std::string> g_webQueue;
constexpr UINT_PTR kFlushTimerId = 77;

void CALLBACK FlushTimerProc(HWND, UINT, UINT_PTR id, DWORD) {
    if (g_app.main) KillTimer(g_app.main, (UINT_PTR)kFlushTimerId);
    Bridge_FlushQueuedEvents();
}

void Bridge_KickFlush() {
    if (g_app.main)
        SetTimer(g_app.main, (UINT_PTR)kFlushTimerId, 30, FlushTimerProc);
}

void Bridge_FlushQueuedEvents() {
    if (g_webQueue.empty()) return;
    std::vector<std::string> batch = std::move(g_webQueue);
    g_webQueue.clear();
    for (auto& j : batch) Wv2_PushEventJson(j);
}

void Bridge_PushEvent(const std::string& jsonUtf8) {
    std::string clean = SanitizeUtf8(jsonUtf8);
    MainWnd_NativeEventSink(clean);
    if (g_webQueue.size() < 128) g_webQueue.push_back(clean);
    Bridge_KickFlush();
}

}  // namespace hs
