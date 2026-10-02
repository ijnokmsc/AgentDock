#include "Launcher.h"
#include "PortableLayout.h"
#include "../platform/ProcessUtil.h"
#include "../platform/PortScanner.h"

#include <cstdlib>
#include <system_error>
#include <fstream>
#include <sstream>
#include <nlohmann/json.hpp>

namespace {

// 注意: 不能命名为 exists —— 与 std::filesystem::exists 在 ADL 下会重载歧义
bool fileExists(const fs::path& p) {
    std::error_code ec;
    return !p.empty() && fs::exists(p, ec);
}

// Hermes 要求数据目录隔离在包内, 这是便携包的核心约定。
//
// 关键: 走「便携包原生链路」—— web-ui 用包根 node 启动后, 会通过 HERMES_BIN
// 找到的 hermes 来自动 spawn Hermes agent bridge (默认监听 18765)。
// 因此必须显式设 HERMES_BIN=<venv>\Scripts\hermes.cmd + HERMES_HOME=data,
// 并**不要**注入 HERMES_WEB_UI_HOME / HERMES_RUNTIME_* 那些桌面版自管 runtime
// 的变量(否则 web-ui 会去用户目录找 desktop-runtime, 而非便携包原生链路)。
//
// 便携化的第二层: 只劫持 HOME/USERPROFILE 是**不够**的。
// Windows 原生消费者(尤其 npm)无视 HOME, 直接读已知文件夹环境变量:
//   npm 在 Windows 上把 cache 硬编码为 %LOCALAPPDATA%\npm-cache,
//   把 global prefix 硬编码为 %APPDATA%\npm;
//   Node 把编译缓存写到 %TEMP%;
//   Python/uv/pip/Chromium 各有自己的 %LOCALAPPDATA% 落点。
// 实测证据(见 docs/PORTABLE-LEAK-AUDIT.md): web-ui 启动时会执行
// `npm prefix --global`, 在宿主 %LOCALAPPDATA%\npm-cache\_logs\ 留下 6 个
// debug 日志 —— 每次启动 6 个, 换台机器就污染一台。
//
// 这一整组变量现在只有**一个真相源**: Paths::sandboxVarTable()
// (PortableBuilder 生成 Hermes.bat 时用同一张表, 两处不会漂移)。

std::vector<std::pair<std::string, std::string>> hermesEnv() {
    // 便携包契约: 沙箱目录先建出来, 并对账 .hermes junction。
    // 之所以每次启动都做: junction 的目标是绝对路径, 便携包换个盘符/目录
    // 就断了 —— 它和 pyvenv.cfg 的 home 是同一类"必须运行期修复"的东西。
    std::string sbErr;
    hs::portable::ensureSandbox(hs::Paths::root(), &sbErr);

    // 沙箱组 (HOME/APPDATA/TEMP/npm/uv/pip/XDG/playwright ...)
    auto env = hs::Paths::sandboxEnvVars();

    // ---- 便携包特有 (非沙箱路径) ----
    env.emplace_back("HERMES_HOME", hs::Paths::dataDir().string());
    env.emplace_back("HERMES_PORTABLE_ROOT", hs::Paths::root().string());
    env.emplace_back("HERMES_PORTABLE_MODE", "1");
    // 显式告诉 web-ui 用便携包的 hermes, 避免它去 PATH / 用户目录找
    env.emplace_back("HERMES_BIN", hs::Paths::hermesExe().string());
    env.emplace_back("PYTHONIOENCODING", "utf-8");
    env.emplace_back("PYTHONUTF8", "1");

    // ---- DSH 集成 (ADR-008): 让 web-ui 的 Agent 管理/Agent 预设发现包内 dsh ----
    // web-ui 探测逻辑 = 在进程 PATH 上找 "dsh" 命令 + DSH_HOME/SourceHome 定位
    // profile。bin/dsh.cmd 垫片由 dsh::ensureDirsAt 生成 (内置包内 DSH_HOME)。
    {
        fs::path dshRoot = hs::Paths::portableRoot() / L"DSH";
        std::error_code ec;
        if (fs::exists(dshRoot / L"bin" / L"dsh.cmd", ec)) {
            std::string dshBin = (dshRoot / L"bin").string();
            std::string oldPath;
            if (const char* p = std::getenv("PATH")) oldPath = p;
            env.emplace_back("PATH", dshBin + ";" + oldPath);
            env.emplace_back("DSH_HOME", (dshRoot / L"data" / L"dsh").string());
        }
    }
    return env;
}

// 从 data/gateway.lock 读取 Hermes agent bridge 的 PID。
// 格式: {"pid": 33456, "kind": "hermes-gateway", "argv": [...], ...}
// web-ui 拉起 agent 后会写这个文件, 比 gateway.pid 更可靠。
std::optional<unsigned long> gatewayPidFromLock() {
    std::error_code ec;
    fs::path lock = hs::Paths::dataDir() / "gateway.lock";
    if (!fs::exists(lock, ec)) return std::nullopt;
    std::ifstream in(lock);
    if (!in) return std::nullopt;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    try {
        auto j = nlohmann::json::parse(text);
        if (j.is_object() && j.contains("pid") && j["pid"].is_number_unsigned()) {
            return j["pid"].get<unsigned long>();
        }
    } catch (...) {}
    return std::nullopt;
}

} // namespace

namespace hs {

Launcher::Launcher() {}

bool Launcher::modeAvailable(StartMode mode) const {
    switch (mode) {
        case StartMode::Cli:     return fileExists(hs::Paths::venvPython()) || fileExists(hs::Paths::hermesExe());
        case StartMode::WebUI:   return fileExists(hs::Paths::nodeExe()) && fileExists(hs::Paths::webUiEntry());
        case StartMode::Desktop: return fileExists(hs::Paths::desktopExe());
    }
    return false;
}

std::string Launcher::modeUnavailableReason(StartMode mode) const {
    switch (mode) {
        case StartMode::Cli:
            return "未找到 Hermes 内核入口 (venv\\Scripts\\python.exe 或 hermes.cmd)";
        case StartMode::WebUI:
            if (!fileExists(hs::Paths::nodeExe()))    return "未找到包内 node.exe";
            if (!fileExists(hs::Paths::webUiEntry())) return "未找到 web-ui 的 server 入口 (webui\\dist\\server\\index.js)";
            return "";
        case StartMode::Desktop:
            return "此便携包未包含桌面版运行时 (desktop\\ 或 runtime\\desktop\\)";
    }
    return "";
}

Launcher::StartResult Launcher::startCli() {
    StartResult r;
    if (!modeAvailable(StartMode::Cli)) {
        r.message = modeUnavailableReason(StartMode::Cli);
        return r;
    }

    // 便携包原生链路。官方 runtime 的 hermes 入口是 hermes.cmd, 内部就是
    // "<venv>\Scripts\python.exe -m hermes_cli.main"; 这里直接用解释器跑模块,
    // 少一层 cmd 包装, 也绕开 .cmd 的引号解析坑。
    proc::LaunchOptions opt;
    fs::path py = hs::Paths::venvPython();
    if (!py.empty()) {
        opt.exe  = py;
        opt.args = {"-m", "hermes_cli.main"};
    } else {
        opt.exe  = hs::Paths::hermesExe();   // .cmd / .exe 都能起 (见 ProcessUtil)
        opt.args = {};
    }
    opt.workingDir = hs::Paths::root();
    opt.newConsole = true;
    opt.env        = hermesEnv();

    std::string err;
    auto pid = proc::launch(opt, &err);
    if (!pid) { r.message = err; return r; }

    kernelPid_ = pid;
    r.ok = true;
    r.kernelPid = pid;
    r.message = "Hermes CLI 已在新窗口启动 (pid " + std::to_string(*pid) + ")";
    return r;
}

Launcher::StartResult Launcher::startWebUi(int port) {
    StartResult r;
    webUiPort_ = port;
    if (!modeAvailable(StartMode::WebUI)) {
        r.message = modeUnavailableReason(StartMode::WebUI);
        return r;
    }

    // 端口已被占用说明上一实例还在, 不重复拉起
    if (auto occ = net::pidByPort(port)) {
        webUiPid_ = occ;
        r.ok = true;
        r.webUiPid = occ;
        r.message = "Web UI 已在运行 (pid " + std::to_string(*occ) + "), 直接打开 " + webUiUrl(port);
        return r;
    }

    // 便携包原生链路: 包根 node 启动 web-ui, 设 HERMES_BIN 让 web-ui 自动拉起
    // Hermes agent bridge (agent 用便携包的 python + hermes-agent, 非 desktop-runtime)。
    proc::LaunchOptions opt;
    opt.exe        = hs::Paths::nodeExe();
    opt.args       = {hs::Paths::webUiEntry().string(), "start", std::to_string(port)};
    opt.workingDir = hs::Paths::root();
    opt.hidden     = true;
    opt.env        = hermesEnv();

    std::string err;
    auto pid = proc::launch(opt, &err);
    if (!pid) { r.message = err; return r; }

    webUiPid_ = pid;
    r.ok = true;
    r.webUiPid = pid;
    r.message = "Web UI 启动中 (pid " + std::to_string(*pid) + "), 稍后访问 " + webUiUrl(port);
    return r;
}

Launcher::StartResult Launcher::startDesktop() {
    StartResult r;
    if (!modeAvailable(StartMode::Desktop)) {
        r.message = modeUnavailableReason(StartMode::Desktop);
        return r;
    }
    proc::LaunchOptions opt;
    opt.exe        = hs::Paths::desktopExe();
    opt.args       = {};
    opt.workingDir = hs::Paths::desktopExe().parent_path();
    opt.env        = hermesEnv();

    std::string err;
    auto pid = proc::launch(opt, &err);
    if (!pid) { r.message = err; return r; }

    desktopPid_ = pid;
    r.ok = true;
    r.desktopPid = pid;
    r.message = "桌面版已启动 (pid " + std::to_string(*pid) + ")";
    return r;
}

Launcher::StartResult Launcher::start(StartMode mode, int webUiPort) {
    mode_ = mode;
    switch (mode) {
        case StartMode::Cli:     return startCli();
        case StartMode::WebUI:   return startWebUi(webUiPort);
        case StartMode::Desktop: return startDesktop();
    }
    StartResult r;
    r.message = "未知启动模式";
    return r;
}

bool Launcher::stop() {
    bool allOk = true;

    // 1) 桌面版
    if (desktopPid_ && proc::isRunning(*desktopPid_)) {
        allOk &= proc::gracefulStop(*desktopPid_, 3000);
    }
    desktopPid_.reset();

    // 2) 内核: 优先用我们记录的 pid, 其次读 gateway.lock / gateway.pid
    std::optional<unsigned long> kpid = kernelPid_;
    if (!kpid || !proc::isRunning(*kpid)) {
        kpid = gatewayPidFromLock();
    }
    if (!kpid || !proc::isRunning(*kpid)) {
        kpid = proc::pidFromFile(hs::Paths::gatewayPidFile());
    }
    if (kpid && proc::isRunning(*kpid)) {
        allOk &= proc::gracefulStop(*kpid, 5000);
    }
    kernelPid_.reset();

    // 3) Web UI: 记录的 pid 或按端口反查 (web-ui 会 spawn agent, 杀掉它的进程树即可)
    std::optional<unsigned long> wpid = webUiPid_;
    if (!wpid || !proc::isRunning(*wpid)) {
        wpid = net::pidByPort(webUiPort_);
    }
    if (wpid && proc::isRunning(*wpid)) {
        allOk &= proc::killTree(*wpid, 3000);
    }
    webUiPid_.reset();

    // 4) 清理残留锁
    std::error_code ec;
    fs::remove(hs::Paths::gatewayPidFile(), ec);
    fs::remove(hs::Paths::dataDir() / "gateway_state.json", ec);
    fs::remove(hs::Paths::hermesLockFile(), ec);

    return allOk;
}

HermesState Launcher::state() const {
    std::optional<unsigned long> kpid = kernelPid_;
    if (!kpid || !proc::isRunning(*kpid)) {
        kpid = gatewayPidFromLock();
    }
    if (!kpid || !proc::isRunning(*kpid)) {
        kpid = proc::pidFromFile(hs::Paths::gatewayPidFile());
    }
    if (kpid && proc::isRunning(*kpid)) return HermesState::Running;

    if (webUiPid_ && proc::isRunning(*webUiPid_)) return HermesState::Running;
    if (desktopPid_ && proc::isRunning(*desktopPid_)) return HermesState::Running;

    return HermesState::Stopped;
}

std::string Launcher::webUiUrl(int port) const {
    return "http://127.0.0.1:" + std::to_string(port);
}

} // namespace hs
