#include "Paths.h"
#include "../storage/JsonStore.h"

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <vector>

namespace {
fs::path g_root;

// ============================ 布局指纹 ============================
//
// OFFICIAL (官方 prebuilt runtime / PortableBuilder 产物):
//     python/base/python.exe  (或 python/venv/Scripts/python.exe)  +  node/node.exe
// 之所以不用 data\ 当判据: 构建过程里 data\ 是最后才建的, 而 autoDetect
// 需要在「刚构建完、还没起过一次」的状态下也能认出来。
bool isOfficialRoot(const fs::path& p) {
    std::error_code ec;
    const bool hasPython =
        fs::exists(p / "python" / "base" / "python.exe", ec) ||
        fs::exists(p / "python" / "venv" / "Scripts" / "python.exe", ec);
    if (!hasPython) return false;
    return fs::exists(p / "node" / "node.exe", ec) ||
           fs::is_directory(p / "webui", ec);
}

// LEGACY (旧第三方搬运包, 仅兼容读取):  data\ + hermes-agent\ + (venv\ | python\)
bool isLegacyRoot(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_directory(p / "data", ec)) return false;
    if (!fs::is_directory(p / "hermes-agent", ec)) return false;
    return fs::is_directory(p / "venv", ec) || fs::is_directory(p / "python", ec);
}

bool looksLikeHermesRoot(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_directory(p, ec)) return false;
    return isOfficialRoot(p) || isLegacyRoot(p);
}

fs::path firstExisting(const std::vector<fs::path>& candidates) {
    std::error_code ec;
    for (const auto& c : candidates) {
        if (!c.empty() && fs::exists(c, ec)) return c;
    }
    return {};
}

fs::path exeDir() {
    wchar_t buf[MAX_PATH] = {0};
    if (GetModuleFileNameW(nullptr, buf, MAX_PATH) == 0) return {};
    return fs::path(buf).parent_path();
}
} // namespace

namespace hs {

bool Paths::setRoot(const fs::path& root) {
    if (root.empty()) return false;
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(root, ec);
    if (!looksLikeHermesRoot(canonical)) return false;
    g_root = canonical;
    return true;
}

fs::path Paths::portableRoot() {
    return exeDir();   // AgentDock.exe 所在目录 = 便携包根 (锚点)
}

fs::path Paths::resolveRoot(const std::string& hermesRootRelative) {
    if (hermesRootRelative.empty()) return {};
    fs::path portable = portableRoot();
    if (portable.empty()) return {};
    fs::path rel = hermesRootRelative;
    // "." (OFFICIAL 默认) 表示内容就在 exe 同级; 也支持 .\Hermes 形式。
    // 绝对路径直接使用。
    fs::path combined = rel.is_absolute() ? rel : portable / rel;
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(combined, ec);
    if (!looksLikeHermesRoot(canonical)) return {};
    return canonical;
}

fs::path Paths::resolveRootFromSettings() {
    auto j = JsonStore::load(studioDir() / "settings.json");
    std::string rel;
    if (j && j->contains("runtime") && (*j)["runtime"].contains("hermesRoot")) {
        rel = (*j)["runtime"]["hermesRoot"].get<std::string>();
    }
    if (rel.empty()) return {};
    return resolveRoot(rel);
}

bool Paths::autoDetect() {
    fs::path here = exeDir();

    // 1) exe 同级 = 内容根 (OFFICIAL 默认布局)
    if (looksLikeHermesRoot(here)) { g_root = here; return true; }

    // 2) 兼容「内容收在子目录」的布局 (exe 在根, 内容在 <root>/Hermes/)
    for (const char* sub : {"Hermes", "hermes"}) {
        fs::path cand = here / sub;
        if (looksLikeHermesRoot(cand)) { g_root = cand; return true; }
    }

    // 3) 逐级向上 (最多 6 层), 兼容 exe 放在子目录的情形
    fs::path p = here;
    for (int i = 0; i < 6 && p.has_parent_path(); ++i) {
        p = p.parent_path();
        if (looksLikeHermesRoot(p)) { g_root = p; return true; }
    }
    return false;
}

const fs::path& Paths::root() { return g_root; }
bool Paths::isValid() { return !g_root.empty() && looksLikeHermesRoot(g_root); }

bool Paths::isOfficialLayout() { return !g_root.empty() && isOfficialRoot(g_root); }
bool Paths::isLegacyLayout()   { return !g_root.empty() && isLegacyRoot(g_root); }

fs::path Paths::dataDir()        { return g_root / "data"; }
// HOME 劫持目标。宿主用 C:\Users\<me> 装下了所有「用户级」状态:
//   .hermes / .cache / .config / .local / .npmrc / .uv / AppData
// 便携版把它们整体搬到 <root>/_home, 使得进程无论从哪个 API 取
// 「用户目录」都只会落在包内。_home\.hermes 是指向 data\ 的 junction。
fs::path Paths::sandboxDir()     { return g_root / "_home"; }
// 启动器自有数据 (设置/模型/日志/备份/下载缓存) 锚定便携包根, 不进 Agent 目录:
// 多 Agent 架构下 Agent 目录会被更新/重建/删除, 启动器数据必须独立存活。
fs::path Paths::studioDir()      { return portableRoot() / "studio"; }
fs::path Paths::pluginDir()      { return portableRoot() / "plugin"; }
fs::path Paths::envFile()        { return g_root / "data" / ".env"; }
fs::path Paths::configFile()     { return g_root / "data" / "config.yaml"; }
fs::path Paths::configTemplate() { return studioDir() / "config.template.yaml"; }
fs::path Paths::logsDir()        { return studioDir() / "logs"; }
fs::path Paths::backupDir()      { return portableRoot() / "studio" / "backups"; }
fs::path Paths::downloadDir()    { return portableRoot() / "studio" / "cache" / "downloads"; }

// ---- 运行时 (布局感知) ----

fs::path Paths::venvDir() {
    // OFFICIAL: <root>/python/venv   (venv 与 hermes-agent 源码同在 python/ 下)
    // LEGACY  : <root>/venv
    std::error_code ec;
    fs::path a = g_root / "python" / "venv";
    if (fs::is_directory(a, ec)) return a;
    fs::path b = g_root / "venv";
    if (fs::is_directory(b, ec)) return b;
    return a;
}

fs::path Paths::venvPython() {
    fs::path v = venvDir();
    return firstExisting({v / "Scripts" / "python.exe", v / "bin" / "python"});
}

fs::path Paths::hermesExe() {
    // 官方 runtime 打包出的入口是 hermes.cmd (不是 .exe)。web-ui 通过
    // HERMES_BIN 拿到它去 spawn agent bridge, 所以要能返回 .cmd。
    fs::path v = venvDir();
    return firstExisting({v / "Scripts" / "hermes.exe",
                          v / "Scripts" / "hermes.cmd",
                          v / "bin" / "hermes"});
}

fs::path Paths::portablePython() {
    // OFFICIAL: python/base/python.exe
    // LEGACY  : python/python.exe | python/cpython-<ver>-<triple>/python.exe
    fs::path direct = firstExisting({g_root / "python" / "base" / "python.exe",
                                     g_root / "python" / "python.exe",
                                     g_root / "python" / "bin" / "python.exe"});
    if (!direct.empty()) return direct;

    std::error_code ec;
    fs::path base = g_root / "python";
    if (!fs::is_directory(base, ec)) return venvPython();
    std::vector<fs::path> found;
    for (fs::directory_iterator it(base, ec), end; it != end; ++it) {
        if (!it->is_directory(ec)) continue;
        if (it->path().filename() == "venv") continue;    // venv 不算解释器安装
        fs::path cand = it->path() / "python.exe";
        if (fs::exists(cand, ec)) found.push_back(cand);
    }
    if (found.empty()) return venvPython();
    // 多个 cpython 目录时取字典序最大者 (版本号靠后的更新)
    std::sort(found.begin(), found.end());
    return found.back();
}

fs::path Paths::nodeExe() {
    return firstExisting({g_root / "node" / "node.exe",
                          g_root / "node" / "bin" / "node.exe"});
}

fs::path Paths::webUiDir() {
    // OFFICIAL: <root>/webui
    // LEGACY  : <root>/node/node_modules/hermes-web-ui
    std::error_code ec;
    fs::path a = g_root / "webui";
    if (fs::is_directory(a, ec)) return a;
    return g_root / "node" / "node_modules" / "hermes-web-ui";
}

fs::path Paths::webUiEntry() {
    return webUiDir() / "dist" / "server" / "index.js";
}

fs::path Paths::desktopExe() {
    // 桌面版运行时并非每个包都有; 找不到就返回空 (Desktop 模式即不可用)
    std::error_code ec;
    for (const char* sub : {"desktop", "runtime/desktop"}) {
        fs::path base = g_root / sub;
        if (!fs::is_directory(base, ec)) continue;
        for (fs::recursive_directory_iterator it(base, ec), end; it != end; ++it) {
            if (!it->is_regular_file(ec) || it->path().extension() != ".exe") continue;
            auto name = it->path().filename().string();
            if (name == "Hermes.exe" || name == "Ekko Studio.exe" || name == "electron.exe") {
                return it->path();
            }
        }
    }
    return {};
}

fs::path Paths::hermesAgentDir() {
    // OFFICIAL: hermes-agent 源码树根**就是** <root>/python (里面有 hermes_cli/)。
    //           注意它同时装着 base/ venv/ —— 做内核更新时必须跳过这两者!
    std::error_code ec;
    if (fs::is_directory(g_root / "python" / "hermes_cli", ec)) return g_root / "python";
    return g_root / "hermes-agent";
}

fs::path Paths::pythonDir()      { return g_root / "python"; }
fs::path Paths::nodeDir()        { return g_root / "node"; }
fs::path Paths::gitDir()         { return g_root / "git"; }

fs::path Paths::ensurePosixTestShimDir() {
    try {
        fs::path dir = studioDir() / "bin";
        fs::create_directories(dir);
        fs::path shim = dir / "[.cmd";
        // 内容保持纯 ASCII (cmd 解析约定); 语义对齐 test(1) 常用单目形式
        const char* body =
            "@echo off\r\n"
            "rem POSIX test(1) shim: npm lifecycle scripts run under cmd.exe where\r\n"
            "rem \"[ -d x ]\" from POSIX-only packages cannot resolve, triggering\r\n"
            "rem fallback builds that need devDependencies (npm install fails).\r\n"
            "setlocal\r\n"
            "set \"HS_T_FLAG=%~1\"\r\n"
            "set \"HS_T_PATH=%~2\"\r\n"
            "if \"%HS_T_FLAG%\"==\"-d\" if exist \"%HS_T_PATH%\\\" exit /b 0\r\n"
            "if \"%HS_T_FLAG%\"==\"-f\" if exist \"%HS_T_PATH%\" if not exist \"%HS_T_PATH%\\\" exit /b 0\r\n"
            "if \"%HS_T_FLAG%\"==\"-e\" if exist \"%HS_T_PATH%\" exit /b 0\r\n"
            "if \"%HS_T_FLAG%\"==\"-n\" if not \"%HS_T_PATH%\"==\"\" exit /b 0\r\n"
            "if \"%HS_T_FLAG%\"==\"-z\" if \"%HS_T_PATH%\"==\"\" exit /b 0\r\n"
            "exit /b 1\r\n";
        std::error_code ec;
        if (!fs::exists(shim, ec)) {
            std::ofstream out(shim, std::ios::binary | std::ios::trunc);
            if (!out) return {};
            out << body;
            if (!out) return {};
        }
        return dir;
    } catch (...) {
        return {};
    }
}
fs::path Paths::runtimeManifestFile() { return g_root / "runtime-manifest.json"; }

fs::path Paths::gatewayPidFile() { return dataDir() / "gateway.pid"; }
fs::path Paths::hermesLockFile() { return dataDir() / ".hermes.lock"; }

// ============================ 沙箱 ============================

const std::vector<Paths::SandboxVar>& Paths::sandboxVarTable() {
    static const std::vector<SandboxVar> table = {
        // ---- 宿主用户目录替身 (POSIX 系消费者: os.homedir() / ~/.hermes / git) ----
        {"HOME",                     "",                     true},
        {"USERPROFILE",              "",                     true},
        {"APPDATA",                  "AppData/Roaming",      true},
        {"LOCALAPPDATA",             "AppData/Local",        true},
        {"TEMP",                     "tmp",                  true},
        {"TMP",                      "tmp",                  true},
        // ---- npm: Windows 上无视 HOME, 必须显式钉住 ----
        // (实测: 不钉会在宿主 %LOCALAPPDATA%\npm-cache\_logs 每次启动留 6 个日志)
        {"NPM_CONFIG_CACHE",         ".npm-cache",           true},
        {"NPM_CONFIG_LOGS_DIR",      ".npm-cache/_logs",     true},
        {"NPM_CONFIG_PREFIX",        "npm-global",           true},
        {"NPM_CONFIG_USERCONFIG",    ".npmrc",               false},
        {"NODE_REPL_HISTORY",        ".node_repl_history",   false},
        // ---- Python 系打包缓存 ----
        {"UV_CACHE_DIR",             ".uv/cache",            true},
        {"PIP_CACHE_DIR",            ".cache/pip",           true},
        {"PYTHONPYCACHEPREFIX",      ".pycache",             true},
        // ---- XDG (跨平台库) ----
        {"XDG_CACHE_HOME",           ".cache",               true},
        {"XDG_CONFIG_HOME",          ".config",              true},
        {"XDG_DATA_HOME",            ".local/share",         true},
        // ---- 浏览器自动化 (web-ui desktop-browser) ----
        {"PLAYWRIGHT_BROWSERS_PATH", ".cache/ms-playwright", true},
    };
    return table;
}

void Paths::createSandboxDirs() {
    createSandboxDirsAt(g_root);
}

void Paths::createSandboxDirsAt(const fs::path& root) {
    if (root.empty()) return;
    std::error_code ec;
    const fs::path sb = root / "_home";
    fs::create_directories(sb, ec);
    // 顺序关键: 目录必须先存在, 再有任何进程把 TEMP/LOCALAPPDATA 指过来。
    // 某些 runtime 在 %TEMP% 不存在时会直接启动失败。
    for (const auto& v : sandboxVarTable()) {
        if (!v.isDir || v.rel == nullptr || v.rel[0] == '\0') continue;
        fs::create_directories(sb / v.rel, ec);   // std::filesystem 在 Windows 上也认 '/'
    }
}

std::vector<std::pair<std::string, std::string>> Paths::sandboxEnvVars() {
    std::vector<std::pair<std::string, std::string>> out;
    if (g_root.empty()) return out;
    const fs::path sb = sandboxDir();
    for (const auto& v : sandboxVarTable()) {
        const bool isRoot = (v.rel == nullptr || v.rel[0] == '\0');
        fs::path val = isRoot ? sb : (sb / v.rel);
        // 表里的 rel 用 '/' 写便于阅读, 但 operator/ 只补分隔符、不会改 rel 内部的
        // '/', 直接 string() 会得到 "_home\AppData/Local" 这种混用路径。
        // 环境变量是要被 git / npm / python 原样解析的, 统一成原生分隔符。
        val.make_preferred();
        out.emplace_back(v.name, val.string());
    }
    return out;
}

} // namespace hs
