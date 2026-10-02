#include "DshRuntime.h"
#include "../platform/ProcessUtil.h"
#include "../platform/PortScanner.h"
#include "../platform/HttpClient.h"

#include <windows.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <cstring>
#include <fstream>
#include <sstream>
#include <nlohmann/json.hpp>

namespace hs::dsh {

namespace {
fs::path g_root;
unsigned long g_procPid = 0;            // start() 记录的 pid (stop 优先用它)

std::mutex   g_tokenMutex;
std::wstring g_tokenUrl;
std::atomic<bool> g_readerLive{false};

// 在一行里找 "http://127.0.0.1:port/?token=..." (dsh web 启动横幅, 纯 ASCII)
void extractTokenUrl(const std::string& line) {
    auto pos = line.find("http://127.0.0.1");
    if (pos == std::string::npos) pos = line.find("http://localhost");
    if (pos == std::string::npos) return;
    auto end = line.find_first_of(" \t\r\n", pos);
    std::string url = line.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    if (url.find("token=") == std::string::npos) return;
    std::wstring w(url.begin(), url.end());
    std::lock_guard<std::mutex> lk(g_tokenMutex);
    g_tokenUrl = w;
}

void trim(std::string& s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
}
} // namespace

void setRoot(const fs::path& r) { g_root = r; }
fs::path root()    { return g_root; }
fs::path nodeExe() { return g_root / L"node" / L"node.exe"; }
fs::path binJs()   { return g_root / L"dsh" / L"node_modules" / L"@deepseek-ai" / L"dsh" / L"lib" / L"bin.js"; }
fs::path homeDir() { return g_root / L"data" / L"dsh"; }
fs::path batPath() { return g_root / L"Dsh.bat"; }

bool installed() {
    std::error_code ec;
    return !g_root.empty() && fs::exists(nodeExe(), ec) && fs::exists(binJs(), ec);
}

// 与 DshBuilder 生成的 Dsh.bat 模板保持同一份事实 (两处不会漂移)
std::vector<std::pair<std::string, std::string>> pinnedEnvAt(const fs::path& root) {
    return {
        { "DSH_HOME",         (root / L"data" / L"dsh").string() },
        { "HOME",             (root / L"_home").string() },
        { "USERPROFILE",      (root / L"_home").string() },
        { "APPDATA",          (root / L"_home" / L"AppData" / L"Roaming").string() },
        { "LOCALAPPDATA",     (root / L"_home" / L"AppData" / L"Local").string() },
        { "TEMP",             (root / L"_home" / L"Temp").string() },
        { "TMP",              (root / L"_home" / L"Temp").string() },
        { "NPM_CONFIG_CACHE", (root / L"studio" / L"cache" / L"npm-cache").string() },
        { "PNPM_HOME",        (root / L"studio" / L"cache" / L"pnpm").string() },
    };
}

std::vector<std::pair<std::string, std::string>> pinnedEnv() { return pinnedEnvAt(g_root); }

static void writeDshShim(const fs::path& root);   // 定义在后 (ensureDirsAt 调用)

void ensureDirsAt(const fs::path& root) {
    std::error_code ec;
    for (fs::path p : { root / L"data" / L"dsh",
                        root / L"_home",
                        root / L"_home" / L"AppData" / L"Roaming",
                        root / L"_home" / L"AppData" / L"Local",
                        root / L"_home" / L"Temp",
                        // shell 常用目录: USERPROFILE 钉到 _home 后, 目录选择框/
                        // 文件面板解析 %USERPROFILE%\Desktop 缺目录会弹"位置不可用"
                        root / L"_home" / L"Desktop",
                        root / L"_home" / L"Documents",
                        root / L"_home" / L"Downloads",
                        root / L"bin",
                        root / L"studio" / L"cache" / L"npm-cache",
                        root / L"studio" / L"cache" / L"pnpm" }) {
        fs::create_directories(p, ec);
        ec.clear();
    }
    writeDshShim(root);
}

// dsh.cmd 垫片: 让任意进程 (Hermes web-ui 的 Agent 管理/Agent 预设等) 把
// 包内 dsh 当作普通命令使用 —— PATH 前置 bin/ 即被发现; DSH_HOME 钉在包内。
// 纯 ASCII + CRLF (cmd 解析约束)。
void writeDshShim(const fs::path& root) {
    fs::path shim = root / L"bin" / L"dsh.cmd";
    static const char* kShim =
        "@echo off\r\n"
        "setlocal\r\n"
        "set \"DSH_PKG=%~dp0..\"\r\n"
        "set \"DSH_HOME=%DSH_PKG%\\data\\dsh\"\r\n"
        "set \"NODE_EXE=%DSH_PKG%\\node\\node.exe\"\r\n"
        "set \"DSH_BIN=%DSH_PKG%\\dsh\\node_modules\\@deepseek-ai\\dsh\\lib\\bin.js\"\r\n"
        "if not exist \"%NODE_EXE%\" exit /b 1\r\n"
        "if not exist \"%DSH_BIN%\" exit /b 1\r\n"
        "\"%NODE_EXE%\" \"%DSH_BIN%\" %*\r\n";
    std::error_code ec;
    if (fs::exists(shim, ec)) {
        // 已存在且内容一致则不重写 (避免无谓 mtime 变化)
        std::ifstream in(shim, std::ios::binary);
        std::string cur((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (cur == kShim) return;
    }
    std::ofstream o(shim, std::ios::binary | std::ios::trunc);
    if (o) o.write(kShim, (std::streamsize)strlen(kShim));
}

void ensureDirs() { ensureDirsAt(g_root); }

Versions queryVersions() {
    Versions v;
    if (!installed()) { v.error = "DSH 未安装 (node.exe 或 bin.js 缺失)"; return v; }

    auto nv = proc::runAndCapture({ nodeExe(), { "--version" } }, 15000);
    if (nv.exitCode == 0) { v.node = nv.output; trim(v.node); }

    proc::LaunchOptions opt;
    opt.exe        = nodeExe();
    opt.args       = { binJs().string(), "--version" };
    opt.workingDir = g_root;
    opt.env        = pinnedEnv();
    auto dv = proc::runAndCapture(opt, 30000);
    if (dv.exitCode == 0) { v.dsh = dv.output; trim(v.dsh); }

    if (v.node.empty() || v.dsh.empty())
        v.error = "版本探测失败 (node=" + v.node + " dsh=" + v.dsh + ")";
    return v;
}

StartResult start(int port, std::string* err) {
    StartResult r;
    if (!installed()) {
        r.message = "DSH 未安装, 请先在构建页构建 .\\DSH";
        if (err) *err = r.message;
        return r;
    }
    if (auto occ = net::pidByPort(port)) {   // 端口已被占 = 上一实例还在, 直接复用
        g_procPid = *occ;
        r.ok = true;
        r.pid = *occ;
        r.message = "DSH Web UI 已在运行 (pid " + std::to_string(*occ) + ")";
        return r;
    }
    ensureDirs();
    resetSession();

    // token 只在 stdout 首行横幅 (每 boot 随机) —— 匿名管道 + 读线程捕获
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        r.message = "CreatePipe 失败";
        if (err) *err = r.message;
        return r;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);   // 读端留在父进程

    proc::LaunchOptions opt;
    opt.exe         = nodeExe();
    opt.args        = { binJs().string(), "web", "--no-open", "--port", std::to_string(port) };
    opt.workingDir  = g_root;
    opt.hidden      = true;
    opt.env         = pinnedEnv();
    opt.hStdoutPipe = wr;

    std::string perr;
    auto pid = proc::launch(opt, &perr);
    CloseHandle(wr);   // 父端先关写柄, 读端才能 EOF
    if (!pid) {
        CloseHandle(rd);
        r.message = "启动 dsh 失败: " + perr;
        if (err) *err = r.message;
        return r;
    }
    g_procPid = *pid;
    r.ok = true;
    r.pid = *pid;
    r.message = "DSH Web UI 启动中 (pid " + std::to_string(*pid) + ")";

    if (!g_readerLive.exchange(true)) {
        std::thread([rd]() {
            char buf[4096];
            std::string pending;
            DWORD n = 0;
            while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0) {
                pending.append(buf, n);
                size_t nl;
                while ((nl = pending.find('\n')) != std::string::npos) {
                    extractTokenUrl(pending.substr(0, nl));
                    pending.erase(0, nl + 1);
                }
                if (pending.size() > (1u << 20)) pending.clear();   // 横幅已错过, 防膨胀
            }
            CloseHandle(rd);
            g_readerLive = false;
        }).detach();
    } else {
        CloseHandle(rd);
    }
    return r;
}

void stop(int port) {
    std::optional<unsigned long> pid;
    if (g_procPid && proc::isRunning(g_procPid)) pid = g_procPid;
    if (!pid || !proc::isRunning(*pid)) pid = net::pidByPort(port);
    if (pid && proc::isRunning(*pid)) proc::killTree(*pid, 5000);
    g_procPid = 0;
    resetSession();
}

bool isUp(int port) { return net::pidByPort(port).has_value(); }

std::wstring tokenUrl() {
    std::lock_guard<std::mutex> lk(g_tokenMutex);
    return g_tokenUrl;
}

void resetSession() {
    std::lock_guard<std::mutex> lk(g_tokenMutex);
    g_tokenUrl.clear();
}

// ---- 插件管理 ----
// profile 名固定 web (启动器 `dsh web` 即 --profile web)。
// dsh 自带打包版 pnpm (实测 11.22, 二进制), add/remove 原样转发;
// store 落点由 pinnedEnv 钉住 (LOCALAPPDATA/PNPM_HOME -> 包内), 实测已验证。
fs::path pluginProfileDir() { return homeDir() / L"profiles" / L"web"; }

std::vector<PluginInfo> pluginList() {
    std::vector<PluginInfo> out;
    std::ifstream in(pluginProfileDir() / L"package.json");
    if (!in) return out;
    try {
        auto j = nlohmann::json::parse(in);
        if (j.contains("dependencies") && j["dependencies"].is_object()) {
            for (auto it = j["dependencies"].begin(); it != j["dependencies"].end(); ++it)
                out.push_back({ it.key(), it.value().is_string() ? it.value().get<std::string>() : "" });
        }
    } catch (...) {}
    return out;
}

static PluginOpResult pluginOp(const char* verb, const std::string& pkg, int timeoutMs);

// pnpm 11 默认拦截依赖构建脚本 (ERR_PNPM_IGNORED_BUILDS), 官方修法是把包名写进
// profile pnpm-workspace.yaml 的 allowBuilds 映射 (pnpm 自身会生成占位
// "<name>: set this to true or false", 这里统一置 true)。用户显式安装该插件
// 即表明需要其原生依赖, 批准是安装语义的一部分。
static void approveBuildScripts(const std::vector<std::string>& pkgs) {
    if (pkgs.empty()) return;
    fs::path ws = pluginProfileDir() / L"pnpm-workspace.yaml";
    std::error_code ec;
    if (!fs::exists(ws, ec)) return;
    std::ifstream in(ws);
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string src = ss.str();
    const std::string orig = src;
    std::string toAppend;
    for (auto& name : pkgs) {
        // 已有条目 (无论真假) 不动; 占位行 "  <name>: set this to true or false" 置 true
        std::string placeholder = "  " + name + ": set this to true or false";
        std::string approved = "  " + name + ": true";
        size_t p = src.find(placeholder);
        if (p != std::string::npos) {
            src.replace(p, placeholder.size(), approved);
        } else if (src.find("  " + name + ":") == std::string::npos) {
            toAppend += approved + "\n";
        }
    }
    if (!toAppend.empty()) {
        size_t key = src.find("allowBuilds:");
        if (key != std::string::npos) {
            size_t insertAt = src.find('\n', key);
            insertAt = (insertAt == std::string::npos) ? src.size() : insertAt + 1;
            src.insert(insertAt, toAppend);
        } else {
            src += "\nallowBuilds:\n" + toAppend;
        }
    }
    if (src == orig) return;
    std::ofstream o(ws, std::ios::binary | std::ios::trunc);
    o << src;
}

// 从 pnpm 输出解析被拦截的构建脚本包名:
// "Ignored build scripts: cloudflared@0.7.3, cpu-features@0.0.10, ..."
static std::vector<std::string> parseIgnoredBuilds(const std::string& output) {
    std::vector<std::string> out;
    auto pos = output.find("Ignored build scripts:");
    if (pos == std::string::npos) return out;
    pos += strlen("Ignored build scripts:");
    auto end = output.find('\n', pos);
    std::string line = output.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    // 每个 token 形如 name@1.2.3; scoped 包名含 '/', 版本从最后一个 '@' 起
    size_t i = 0;
    while (i < line.size()) {
        size_t j = line.find(',', i);
        if (j == std::string::npos) j = line.size();
        std::string tok = line.substr(i, j - i);
        // trim
        size_t b = tok.find_first_not_of(" \t\r");
        size_t e = tok.find_last_not_of(" \t\r");
        if (b != std::string::npos) {
            tok = tok.substr(b, e - b + 1);
            size_t at = tok.rfind('@');
            std::string name = (at != std::string::npos && at > 0) ? tok.substr(0, at) : tok;
            if (!name.empty()) out.push_back(name);
        }
        i = j + 1;
    }
    return out;
}

static PluginOpResult pluginOp(const char* verb, const std::string& pkg, int timeoutMs) {
    PluginOpResult r;
    if (!installed()) { r.output = "DSH not installed"; return r; }
    if (pkg.empty() || pkg.find_first_of(" \t\"'") != std::string::npos) {
        r.output = "invalid package name"; return r;
    }
    proc::LaunchOptions opt;
    opt.exe        = nodeExe();
    opt.args       = { binJs().string(), "plugin", "--profile", "web", verb, pkg };
    opt.workingDir = g_root;
    opt.hidden     = true;
    opt.env        = pinnedEnv();
    // github:/git+ 来源的插件依赖 pnpm 的 git 拉取; DSH 包未带 git 时
    // 借用同便携包根下 Hermes 子包的 PortableGit (存在则前置 PATH)
    {
        fs::path gitCmd = g_root.parent_path() / L"Hermes" / L"git" / L"cmd";
        std::error_code ec;
        if (fs::exists(gitCmd / L"git.exe", ec)) {
            for (auto& kv : opt.env)
                if (kv.first == "PATH") { kv.second = gitCmd.string() + ";" + kv.second; break; }
        }
    }
    int code = 0;
    auto cap = proc::runAndCapture(opt, timeoutMs, nullptr);
    code = cap.exitCode;
    r.output = cap.output;
    if (r.output.size() > 2000) r.output = r.output.substr(r.output.size() - 2000);
    r.ok = code == 0;
    // 构建脚本被拦截: 批准后自动重试一次 (pnpm 会中断安装并报错, 依赖已部分落盘)
    if (!r.ok && r.output.find("ERR_PNPM_IGNORED_BUILDS") != std::string::npos) {
        auto ignored = parseIgnoredBuilds(r.output);
        if (!ignored.empty()) {
            approveBuildScripts(ignored);
            code = 0;
            cap = proc::runAndCapture(opt, timeoutMs, nullptr);
            code = cap.exitCode;
            r.output = cap.output;
            if (r.output.size() > 2000) r.output = r.output.substr(r.output.size() - 2000);
            r.ok = code == 0;
        }
    }
    return r;
}

static bool isGitSpec(const std::string& s) {
    return s.rfind("github:", 0) == 0 || s.rfind("git+", 0) == 0 ||
           s.rfind("git@", 0) == 0 || s.find("://") != std::string::npos;
}

PluginOpResult pluginAdd(const std::string& pkg) {
    // Git 仓库形来源: pnpm 回退 git clone 的场景需要独立 pnpm, 缺失则自动装入包内
    if (isGitSpec(pkg) && pnpmVersion().empty()) installPnpm();
    return pluginOp("add", pkg, 10 * 60 * 1000);
}
PluginOpResult pluginRemove(const std::string& pkg) { return pluginOp("remove", pkg, 5 * 60 * 1000); }

// ---- pnpm / git 自检与自装 ----
// dsh 自带打包版 pnpm 处理常规安装; Git 仓库形来源 (github:/git+/git@) 在
// pnpm 回退 git clone 的场景需要独立 pnpm/git。包内 pnpm 装到 node/ 根
// (npm -g --prefix, Dsh.bat 与 PATH 注入均覆盖该目录, 不污染宿主)。
static bool fileExistsP(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

bool gitAvailable() {
    wchar_t buf[MAX_PATH];
    if (SearchPathW(nullptr, L"git.exe", nullptr, MAX_PATH, buf, nullptr) > 0) return true;
    return fileExistsP(g_root.parent_path() / L"Hermes" / L"git" / L"cmd" / L"git.exe");
}

static std::string queryPnpm(const fs::path& pnpmCmd) {
    proc::LaunchOptions opt;
    opt.exe  = pnpmCmd;
    opt.args = { "--version" };
    auto r = proc::runAndCapture(opt, 20000);
    if (r.exitCode != 0) return "";
    std::string v = r.output;
    while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
    return v;
}

std::string pnpmVersion() {
    std::error_code ec;
    // 1) 包内 (npm -g --prefix 的落点)
    fs::path inPkg = g_root / L"node" / L"pnpm.cmd";
    if (fileExistsP(inPkg)) {
        std::string v = queryPnpm(inPkg);
        if (!v.empty()) return v;
    }
    // 2) 系统 PATH
    wchar_t buf[MAX_PATH];
    if (SearchPathW(nullptr, L"pnpm.cmd", nullptr, MAX_PATH, buf, nullptr) > 0 ||
        SearchPathW(nullptr, L"pnpm.exe", nullptr, MAX_PATH, buf, nullptr) > 0)
        return queryPnpm(buf);
    return "";
}

PluginOpResult installPnpm() {
    PluginOpResult r;
    if (!installed()) { r.output = "DSH not installed"; return r; }
    auto existing = pnpmVersion();
    if (!existing.empty()) { r.ok = true; r.output = existing; return r; }

    proc::LaunchOptions opt;
    opt.exe  = nodeExe();
    opt.args = { (g_root / L"node" / L"node_modules" / L"npm" / L"bin" / L"npm-cli.js").string(),
                 "install", "-g", "pnpm", "--prefix", (g_root / L"node").string(),
                 "--loglevel=error" };
    opt.workingDir = g_root;
    opt.hidden     = true;
    opt.env        = pinnedEnv();
    auto cap = proc::runAndCapture(opt, 5 * 60 * 1000);
    r.output = cap.output;
    if (r.output.size() > 2000) r.output = r.output.substr(r.output.size() - 2000);
    r.ok = cap.exitCode == 0 && fileExistsP(g_root / L"node" / L"pnpm.cmd");
    if (r.ok) r.output = pnpmVersion();   // 复用字段带出版本号
    return r;
}

bool httpReady(int port) {
    net::HttpOptions opt;
    opt.timeoutSec = 2;
    auto resp = net::httpGet("http://127.0.0.1:" + std::to_string(port) + "/", opt);
    return resp.status != 0;   // 401 fence / 303 均算就绪, 连不上才算未就绪
}

} // namespace hs::dsh
