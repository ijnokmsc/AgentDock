// 体积优化插件 v0.3: 扫描整个便携包目录, 分类统计可清理的垃圾文件,
// 标注清理风险等级, 支持按类别勾选清理; 一键清理默认只动无风险项。
//
// 便携包布局锚点: AgentDock.exe 所在目录 = 便携包根 (可能内嵌多个同构包:
// 便携包根本身 + DSH\ + Hermes\ 各有 studio/_home/data/WebViewData 布局,
// 规则按相对路径后缀匹配, 天然覆盖所有嵌套实例)。
//
// 分类与风险 (risk: 0=无风险 默认勾选, 1=低风险 可重建但需重新下载, 2=高风险):
//   upd_dl   更新下载缓存     studio\cache\downloads            (重下载即可)
//   backups  更新回滚备份     studio\backups                    (删后无法回滚)
//   pkgcache 包管理器缓存     pnpm store/npm-cache/node-gyp/pip/uv (需重新联网下载)
//   datacache 数据缓存        data\cache 等模型/音频/图片缓存    (自动重建)
//   webcache Web/浏览器缓存   WebViewData 各 Cache + web-ui cache (自动重建, 不动登录态)
//   logs     日志文件         *\.logs 目录 + *.log               (重跑即有)
//   pycache  Python 编译缓存  __pycache__/.pytest_cache 等        (自动重建)
//   temp     临时与崩溃转储   AppData\Local\Temp + *.tmp/*.dmp
//
// 结果回传: 扫描/清理的结构化数据经日志行 "@@DATA@@{json}" 交给宿主
// (HSPP v1.4 约定, 宿主抽出后随 plugin.action.result.data 回 UI),
// 人读摘要仍走普通日志尾部 (悬浮菜单/概览卡直接展示)。
//
// 编译: cl /LD /I<hs_plugin.h 目录> clear_logs_plugin.cpp /Fe:plugin.dll /link /EXPORT:hs_plugin_entry

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>
#include "hs_plugin.h"

#ifdef _WIN32
#define EXPORT extern "C" __declspec(dllexport)
#else
#define EXPORT extern "C"
#endif

static const hs_host_api_vtable* g_host = nullptr;

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
static std::string lower(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c += 32;
    return s;
}
static bool endsWith(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}
static bool contains(const std::string& s, const std::string& sub) {
    return s.find(sub) != std::string::npos;
}
static std::string fmtSize(unsigned long long b) {
    char buf[64];
    if (b >= 1ull << 30) { snprintf(buf, sizeof buf, "%.2f GB", (double)b / (1ull << 30)); return buf; }
    if (b >= 1ull << 20) { snprintf(buf, sizeof buf, "%.1f MB", (double)b / (1ull << 20)); return buf; }
    if (b >= 1ull << 10) { snprintf(buf, sizeof buf, "%.1f KB", (double)b / (1ull << 10)); return buf; }
    snprintf(buf, sizeof buf, "%llu B", b);
    return buf;
}
static std::string escapeJson(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; }
            else out += c;
        }
    }
    return out;
}
static void plog(int lvl, const std::string& msg) {
    if (g_host && g_host->log) g_host->log((hs_log_level)lvl, "cleanup", msg.c_str());
}
// 结构化结果 (HSPP v1.4): 单行日志标记, 宿主抽出后不进人读尾部
static void emitData(const std::string& json) { plog(HS_LOG_INFO, "@@DATA@@" + json); }

// 便携包根探测 (按可靠度降级):
//   1) 宿主进程 exe 所在目录 —— 插件进程内加载, GetModuleFileNameA(NULL) 即
//      AgentDock.exe 路径, 与宿主 Paths::portableRoot (= exeDir) 同一锚点,
//      不依赖任何配置 (settings.json 可能没有 hermesRoot);
//   2) hermesRoot 配置向上找 AgentDock.exe (老路径兜底, 兼容独立加载/测试桩)。
// 候选需过哨兵检查 (含 plugin/ 或 studio/ 目录或 AgentDock.exe), 防清错树。
static bool saneRoot(const std::string& dir) {
    if (dir.empty()) return false;
    return GetFileAttributesA((dir + "\\plugin").c_str()) != INVALID_FILE_ATTRIBUTES ||
           GetFileAttributesA((dir + "\\studio").c_str()) != INVALID_FILE_ATTRIBUTES ||
           GetFileAttributesA((dir + "\\AgentDock.exe").c_str()) != INVALID_FILE_ATTRIBUTES;
}
static std::string parentDir(std::string p) {
    size_t pos = p.find_last_of("\\/");
    if (pos == std::string::npos || pos == 0) return "";
    return p.substr(0, pos);
}
static std::string portableRoot() {
    // 1) 宿主 exe 目录
    char buf[MAX_PATH]{};
    if (GetModuleFileNameA(nullptr, buf, MAX_PATH)) {
        std::string dir = parentDir(buf);
        if (saneRoot(dir)) return dir;
    }
    // 2) hermesRoot (可能是相对路径, 先按宿主 exe 目录拼绝对) 向上找 AgentDock.exe
    std::string root;
    if (g_host && g_host->config_get) {
        hs_str out = nullptr;
        if (g_host->config_get("hermesRoot", &out) == 0 && out) {
            root = out;
            if (!root.empty() && root.front() == '"') root = root.substr(1);
            if (!root.empty() && root.back() == '"') root.pop_back();
            g_host->string_free(out);
        }
    }
    if (!root.empty() && (root.back() == '\\' || root.back() == '/')) root.pop_back();
    if (!root.empty() && root[0] != '\\' && (root.size() < 2 || root[1] != ':')) {
        std::string base = parentDir(buf);           // 相对配置: 锚到宿主 exe 目录
        if (!base.empty()) root = base + "\\" + root;
    }
    std::string probe = root;
    for (int i = 0; i < 4 && !probe.empty(); ++i) {
        if (GetFileAttributesA((probe + "\\AgentDock.exe").c_str()) != INVALID_FILE_ATTRIBUTES)
            return probe;
        probe = parentDir(probe);
    }
    if (saneRoot(root)) return root;                 // 内容根本身 (内容在 exe 同级的布局)
    // 3) 本 DLL 位置向上 (<根>\plugin\<id>\plugin.dll)
    HMODULE hm = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&portableRoot, &hm) &&
        GetModuleFileNameA(hm, buf, MAX_PATH)) {
        std::string p = buf;
        for (int i = 0; i < 3; ++i) p = parentDir(p);   // clear-logs -> plugin -> <根>
        if (saneRoot(p)) return p;
    }
    return "";
}

// 递归删除目录下所有条目 (保留目录本身)。返回删除的文件数。
static int clearDirKeep(const std::string& dir) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int deleted = 0;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        std::string full = dir + "\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            deleted += clearDirKeep(full), RemoveDirectoryA(full.c_str());
        else { DeleteFileA(full.c_str()); ++deleted; }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return deleted;
}
// 删除整棵目录树 (含目录本身)
static void removeTree(const std::string& dir) {
    clearDirKeep(dir);
    RemoveDirectoryA(dir.c_str());
}

// ---------------------------------------------------------------------------
// 分类定义与匹配规则
// ---------------------------------------------------------------------------
enum { C_UPD = 0, C_BAK, C_PKG, C_DATA, C_WEB, C_LOGS, C_PYC, C_TMP, C_CATN };

struct CatDef { const char* id; const char* name; int risk; const char* note; };
static const CatDef kCats[C_CATN] = {
    { "upd_dl",   "更新下载缓存",      0, "组件更新包, 下次更新时重新下载" },
    { "backups",  "更新回滚备份",      2, "删除后无法一键回滚到旧版本" },
    { "pkgcache", "包管理器缓存",      1, "npm/pnpm/node-gyp/pip 等下载缓存, 清空后装依赖需重新联网下载" },
    { "datacache","数据缓存",          0, "模型目录/音频/图片等数据缓存, 运行时自动重建" },
    { "webcache", "Web/浏览器缓存",    0, "WebView2 与 web-ui 缓存, 下次启动自动重建, 不影响登录状态" },
    { "logs",     "日志文件",          0, "各组件运行日志, 需要排障时重新生成" },
    { "pycache",  "Python 编译缓存",   0, "__pycache__ 等字节码缓存, 运行时自动重建" },
    { "temp",     "临时文件与崩溃转储", 0, "临时目录与 *.tmp/*.dmp, 可安全删除" },
};

// 目录命中后的清理方式: 删整树(+重建空目录) / 清空内容保留目录
enum { M_DELTREE = 0, M_DELTREE_KEEPDIR = 1, M_CLEARKEEP = 2, M_FILE = 3 };
static int catDirMode[C_CATN] = { M_DELTREE_KEEPDIR, M_DELTREE_KEEPDIR, M_DELTREE_KEEPDIR,
                                  M_DELTREE_KEEPDIR, M_DELTREE, M_CLEARKEEP, M_DELTREE, M_CLEARKEEP };

struct DirRule { const char* suffix; int cat; };
// 顺序即优先级: 具体前缀在前, 通用的 \logs 兜底在后
static const DirRule kDirRules[] = {
    { "\\studio\\cache\\downloads",        C_UPD },
    { "\\studio\\cache\\pnpm",             C_PKG },
    { "\\studio\\cache\\npm-cache",        C_PKG },
    { "\\studio\\backups",                 C_BAK },
    { "\\appdata\\local\\npm-cache",       C_PKG },
    { "\\appdata\\local\\node-gyp",        C_PKG },
    { "\\appdata\\local\\pip\\cache",      C_PKG },
    { "\\appdata\\local\\uv\\cache",       C_PKG },
    { "\\appdata\\local\\pnpm-cache",      C_PKG },
    { "\\.npm\\_cacache",                  C_PKG },
    { "\\appdata\\local\\temp",            C_TMP },
    { "\\_home\\tmp",                      C_TMP },
    { "\\__pycache__",                     C_PYC },
    { "\\.pytest_cache",                   C_PYC },
    { "\\.mypy_cache",                     C_PYC },
    { "\\.ruff_cache",                     C_PYC },
    { "\\data\\cache",                     C_DATA },
    { "\\data\\audio_cache",               C_DATA },
    { "\\data\\image_cache",               C_DATA },
    { "\\.hermes-web-ui\\cache",           C_WEB },
    { "\\logs",                            C_LOGS },   // 通用兜底放最后
};
static const int kDirRuleN = sizeof(kDirRules) / sizeof(kDirRules[0]);

// WebViewData 内允许清理的缓存目录段 (Service Worker/IndexedDB/Cookies 等用户数据绝不碰)
static const char* kWebSegs[] = {
    "cache", "code cache", "gpucache", "dawngraphitecache", "dawnwebgpucache",
    "shadercache", "grshadercache", "media cache", "crashpad", "component_crx_cache",
};

static std::string lastSeg(const std::string& rel) {
    size_t p = rel.find_last_of('\\');
    return p == std::string::npos ? rel : rel.substr(p + 1);
}

// 目录规则命中: 返回类别索引, 未命中 -1。
// 规则后缀都带前导 '\', 这里给相对路径补上前导分隔符,
// 使便携包根直下的目录 (rel 无前缀) 也能命中。
static int matchDir(const std::string& relLower) {
    std::string pre = "\\" + relLower;
    for (int i = 0; i < kDirRuleN; ++i)
        if (endsWith(pre, kDirRules[i].suffix)) return kDirRules[i].cat;
    if (contains(pre, "\\webviewdata\\")) {
        std::string seg = lastSeg(relLower);
        for (const char* w : kWebSegs)
            if (seg == w) return C_WEB;
    }
    return -1;
}

// 文件规则命中: 返回类别索引, 未命中 -1
static int matchFile(const std::string& relLower) {
    if (endsWith(relLower, ".log")) return C_LOGS;
    if (endsWith(relLower, ".pyc") || endsWith(relLower, ".pyo")) return C_PYC;
    if (endsWith(relLower, ".tmp") || endsWith(relLower, ".dmp")) return C_TMP;
    // 任意 data\ 目录下的模型目录缓存文件 (models_dev_cache.json / *_cache.etag / ...)
    std::string pre = "\\" + relLower;
    if (contains(pre, "\\data\\") &&
        (endsWith(relLower, "_cache.json") || endsWith(relLower, "_cache.etag") ||
         endsWith(relLower, ".etag")))
        return C_DATA;
    return -1;
}

// ---------------------------------------------------------------------------
// 扫描引擎
// ---------------------------------------------------------------------------
struct Item {
    std::string path;      // 绝对路径 (清理动作对象)
    int mode;
};
struct ScanResult {
    std::string root;
    unsigned long long bytes[C_CATN] = {};
    int files[C_CATN] = {};
    std::vector<Item> items[C_CATN];
    int scannedFiles = 0;
    unsigned long long scannedBytes = 0;
    int durMs = 0;
};

static bool isReparse(DWORD attr) { return (attr & FILE_ATTRIBUTE_REPARSE_POINT) != 0; }

// 量一棵树的体积 (跳过 reparse 链接, 不套规则)
static void measureTree(const std::string& dir, unsigned long long& bytes, int& files) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        if (isReparse(fd.dwFileAttributes)) continue;
        std::string full = dir + "\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) measureTree(full, bytes, files);
        else {
            unsigned long long sz = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            bytes += sz; ++files;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// 规则遍历: node_modules/.git 整体跳过 (运行时/仓库, 无垃圾且量大),
// 规则命中的目录量完体积后不再下探 (内容整体归属该类别)。
static void walk(const std::string& dir, const std::string& rel, ScanResult& r) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        if (isReparse(fd.dwFileAttributes)) continue;   // pnpm junction/符号链接不深入不计量
        std::string name = fd.cFileName;
        std::string full = dir + "\\" + name;
        std::string rl = lower(rel.empty() ? name : rel + "\\" + name);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            std::string lname = lower(name);
            if (lname == "node_modules" || lname == ".git") continue;
            int cat = matchDir(rl);
            if (cat >= 0) {
                unsigned long long b = 0; int f = 0;
                measureTree(full, b, f);
                r.bytes[cat] += b; r.files[cat] += f;
                if (r.items[cat].size() < 256) r.items[cat].push_back({ full, catDirMode[cat] });
                r.scannedBytes += b; r.scannedFiles += f;
            } else {
                walk(full, rl, r);
            }
        } else {
            unsigned long long sz = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            r.scannedBytes += sz; ++r.scannedFiles;
            int cat = matchFile(rl);
            if (cat >= 0) {
                r.bytes[cat] += sz; ++r.files[cat];
                if (r.items[cat].size() < 1024) r.items[cat].push_back({ full, M_FILE });
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// 执行扫描。root 为空返回 false。
static bool scan(ScanResult& r) {
    r.root = portableRoot();
    if (r.root.empty()) return false;
    unsigned long long t0 = GetTickCount64();
    walk(r.root, "", r);
    r.durMs = (int)(GetTickCount64() - t0);
    return true;
}

// 勾选清单回 UI (HSPP v1.4 checklist 约定): 风险 0 项由 UI 默认勾选
static void emitScanData(const ScanResult& r) {
    std::string items;
    for (int c = 0; c < C_CATN; ++c) {
        if (r.files[c] == 0) continue;                     // 空类别不上列表
        if (!items.empty()) items += ",";
        items += std::string("{\"id\":\"") + kCats[c].id + "\",\"name\":\"" +
                 escapeJson(kCats[c].name) + "\",\"risk\":" + std::to_string(kCats[c].risk) +
                 ",\"bytes\":" + std::to_string(r.bytes[c]) +
                 ",\"files\":" + std::to_string(r.files[c]) +
                 ",\"note\":\"" + escapeJson(kCats[c].note) + "\",\"targets\":[";
        std::string tg;
        int n = 0;
        for (auto& it : r.items[c]) {
            if (n >= 4) break;
            std::string rel = it.path;
            if (rel.size() > r.root.size() + 1) rel = rel.substr(r.root.size() + 1);
            if (!tg.empty()) tg += ",";
            tg += "\"" + escapeJson(rel) + "\"";
            ++n;
        }
        items += tg + "]}";
    }
    emitData("{\"checklist\":{\"root\":\"" + escapeJson(r.root) +
             "\",\"scannedFiles\":" + std::to_string(r.scannedFiles) +
             ",\"scannedBytes\":" + std::to_string(r.scannedBytes) +
             ",\"durMs\":" + std::to_string(r.durMs) +
             ",\"items\":[" + items + "]}}");
}

static unsigned long long riskBytes(const ScanResult& r, int risk) {
    unsigned long long s = 0;
    for (int c = 0; c < C_CATN; ++c) if (kCats[c].risk == risk) s += r.bytes[c];
    return s;
}

// 从 payload 提取 {"categories":[...]} 字符串数组 (迷你解析, 不引依赖)
static std::vector<std::string> parseCategories(const std::string& payload) {
    std::vector<std::string> out;
    size_t p = payload.find("\"categories\"");
    if (p == std::string::npos) return out;
    p = payload.find('[', p);
    if (p == std::string::npos) return out;
    bool inStr = false;
    std::string cur;
    for (size_t i = p + 1; i < payload.size(); ++i) {
        char c = payload[i];
        if (inStr) {
            if (c == '\\') { if (i + 1 < payload.size()) cur += payload[++i]; }
            else if (c == '"') { inStr = false; out.push_back(cur); cur.clear(); }
            else cur += c;
        } else {
            if (c == '"') inStr = true;
            else if (c == ']') break;
        }
    }
    return out;
}

// 动作: 扫描。输出清单数据 + 人读摘要。
static std::string doScan() {
    ScanResult r;
    if (!scan(r)) return "无法确定便携包根目录 (hermesRoot 为空且未找到 AgentDock.exe)";
    emitScanData(r);
    unsigned long long total = 0; int kinds = 0;
    for (int c = 0; c < C_CATN; ++c) if (r.files[c]) { total += r.bytes[c]; ++kinds; }
    plog(HS_LOG_INFO, "扫描 " + r.root + " 完成: " + std::to_string(r.scannedFiles) +
         " 项 / " + fmtSize(r.scannedBytes) + ", 耗时 " + std::to_string(r.durMs) + "ms");
    plog(HS_LOG_INFO, "发现 " + std::to_string(kinds) + " 类垃圾共 ~" + fmtSize(total) +
         " (无风险 ~" + fmtSize(riskBytes(r, 0)) + ", 低风险 ~" + fmtSize(riskBytes(r, 1)) +
         ", 高风险 ~" + fmtSize(riskBytes(r, 2)) + ")");
    return "";
}

// 动作: 按类别清理 (cats 为空 = 只清无风险)。先重新扫描保证路径新鲜。
static std::string doClean(const std::vector<std::string>& cats) {
    ScanResult r;
    if (!scan(r)) return "无法确定便携包根目录 (hermesRoot 为空且未找到 AgentDock.exe)";
    bool byId[C_CATN] = {};
    if (cats.empty()) for (int c = 0; c < C_CATN; ++c) byId[c] = (kCats[c].risk == 0);
    else for (auto& id : cats)
        for (int c = 0; c < C_CATN; ++c)
            if (id == kCats[c].id) byId[c] = true;

    unsigned long long freed = 0;
    int nfiles = 0, ncats = 0;
    std::string cleanedCats, detail;
    for (int c = 0; c < C_CATN; ++c) {
        if (!byId[c] || r.items[c].empty()) continue;
        int f = 0;
        for (auto& it : r.items[c]) {
            switch (it.mode) {
            case M_DELTREE:       removeTree(it.path); ++f; break;
            case M_DELTREE_KEEPDIR: removeTree(it.path); CreateDirectoryA(it.path.c_str(), nullptr); ++f; break;
            case M_CLEARKEEP:     f += clearDirKeep(it.path); break;
            case M_FILE:          if (DeleteFileA(it.path.c_str())) ++f; break;
            }
        }
        freed += r.bytes[c]; nfiles += f; ++ncats;
        if (!cleanedCats.empty()) cleanedCats += ",";
        cleanedCats += std::string("{\"id\":\"") + kCats[c].id + "\",\"name\":\"" +
                       escapeJson(kCats[c].name) + "\",\"bytes\":" + std::to_string(r.bytes[c]) +
                       ",\"files\":" + std::to_string(f) + "}";
        plog(HS_LOG_INFO, std::string("已清理 ") + kCats[c].name + ": " +
             std::to_string(f) + " 项 (~" + fmtSize(r.bytes[c]) + ")");
    }
    emitData("{\"cleaned\":{\"bytes\":" + std::to_string(freed) +
             ",\"files\":" + std::to_string(nfiles) + ",\"cats\":[" + cleanedCats + "]}}");
    std::string scope = cats.empty() ? "一键清理 (无风险)" : "清理所选";
    plog(HS_LOG_INFO, scope + "完成: " + std::to_string(ncats) + " 类 / " +
         std::to_string(nfiles) + " 项 / ~" + fmtSize(freed));
    return "";
}

// ---------------------------------------------------------------------------
// 插件 ABI
// ---------------------------------------------------------------------------
static void get_info(hs_str* name, hs_str* ver, hs_str* desc) {
    if (name) *name = "clear-logs";
    if (ver)  *ver  = "0.3.0";
    if (desc) *desc = "扫描便携包全目录, 分类统计垃圾文件并标注风险, 默认只清无风险项";
}

static hs_i32 create(const hs_host_api_vtable* host, void** ud) {
    g_host = host;
    if (ud) *ud = (void*)1;
    if (host && host->log) host->log(HS_LOG_INFO, "cleanup", "体积优化插件 v0.3 已加载 (全包扫描)");
    return 0;
}

static void destroy(void* ud) {
    (void)ud;
    if (g_host && g_host->log) g_host->log(HS_LOG_INFO, "cleanup", "体积优化插件已卸载");
}

static hs_event_result on_event(void* ud, hs_event ev, hs_str payload) {
    (void)ud; (void)ev; (void)payload;
    return HS_EVENT_OK;
}

static void get_ui_entries(void* ud, void (*sink)(const hs_ui_entry*, void*), void* ctx) {
    (void)ud;
    hs_ui_entry e;
    e.id = "cleanup.main";
    e.title = "体积优化";
    e.icon = "";
    e.json_schema = "";
    e.default_config = "";
    // HSPP v1.4: scan 的结果带 checklist 数据 (UI 渲染勾选列表);
    // clean 声明 arg=checklist, UI 把勾选类别经 payload.categories 回传。
    e.actions = "["
        "{\"id\":\"clean-safe\",\"label\":\"一键清理 (无风险)\","
        "\"confirm\":\"一键清理全部无风险垃圾 (日志/各类缓存/更新下载包)? 它们运行时自动重建; 高风险项不会触碰。\"},"
        "{\"id\":\"scan\",\"label\":\"扫描垃圾文件\",\"quick\":false},"
        "{\"id\":\"clean\",\"label\":\"清理勾选项\",\"quick\":false,\"arg\":\"checklist\","
        "\"confirm\":\"确认清理勾选的垃圾文件? 此操作不可恢复 (含勾选的高风险项)。\"}"
        "]";
    e.sidebar = 1;
    if (sink) sink(&e, ctx);
}

static hs_i32 execute_action(void* ud, hs_str action_id, hs_str payload) {
    (void)ud;
    if (!action_id) return -1;
    std::string err;
    if (strcmp(action_id, "scan") == 0) {
        err = doScan();
    } else if (strcmp(action_id, "clean-safe") == 0) {
        err = doClean({});
    } else if (strcmp(action_id, "clean") == 0) {
        err = doClean(parseCategories(payload ? payload : ""));
    } else {
        return -1;
    }
    if (!err.empty()) { plog(HS_LOG_ERROR, err); return 1; }
    return 0;
}

static const hs_plugin_api_vtable g_api = {
    HS_ABI_VERSION,
    get_info,
    create,
    destroy,
    on_event,
    get_ui_entries,
    execute_action,
};

EXPORT const hs_plugin_api_vtable* hs_plugin_entry(void) {
    return &g_api;
}
