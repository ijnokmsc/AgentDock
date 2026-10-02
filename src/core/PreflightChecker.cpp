#include "PreflightChecker.h"
#include "ModelRegistry.h"
#include "../platform/PortScanner.h"
#include "../platform/ProcessUtil.h"

#include <fstream>
#include <sstream>
#include <algorithm>

namespace {

bool fileExists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

// 检测 editable install 的 finder 文件是否指向当前根目录
// 便携包被移动后, pip install -e 写入的绝对路径会失效,
// 表现为 "No module named 'hermes_cli'" —— 这是历史坑位, 必须自动修。
bool editablePathsNeedFix() {
    std::error_code ec;
    fs::path sp = hs::Paths::root() / "venv" / "Lib" / "site-packages";
    if (!fs::is_directory(sp, ec)) return false;

    std::string rootStr = hs::Paths::root().string();
    std::transform(rootStr.begin(), rootStr.end(), rootStr.begin(),
                   [](unsigned char c) { return (char)::tolower(c); });

    for (fs::directory_iterator it(sp, ec), end; it != end; ++it) {
        const auto& name = it->path().filename().string();
        if (name.rfind("__editable___hermes_agent", 0) != 0) continue;
        if (it->path().extension() != ".py") continue;

        std::ifstream in(it->path());
        if (!in) continue;
        std::ostringstream ss; ss << in.rdbuf();
        std::string text = ss.str();
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return (char)::tolower(c); });

        // 文件里记录的 hermes-agent 路径不在当前根目录下 => 需要修复
        if (text.find("hermes-agent") == std::string::npos) continue;
        if (text.find(rootStr) == std::string::npos) return true;
    }
    return false;
}
} // namespace

namespace hs {

PreflightChecker::PreflightChecker(Context ctx) : ctx_(std::move(ctx)) {}

std::vector<CheckItem> PreflightChecker::runAll() {
    last_.clear();

    last_.push_back(checkRootLayout());
    last_.push_back(checkRuntime());
    last_.push_back(checkConfigFiles());

    // 模型相关校验交给 ModelRegistry
    ModelRegistry tmp(hs::Paths::studioDir() / "models.json");
    if (ctx_.haveModels) {
        (void)tmp.load();
        for (auto& c : tmp.validate()) last_.push_back(c);
    } else {
        CheckItem c;
        c.id = "models.missing";
        c.title = "模型配置尚未初始化";
        c.level = CheckLevel::Fail;
        c.detail = "未找到 studio/models.json, 需要先从旧配置迁移或手动添加模型";
        c.fixable = true;
        c.fixAction = "goto:models";
        last_.push_back(c);
    }

    last_.push_back(checkPorts());
    last_.push_back(checkEditablePaths());
    last_.push_back(checkStaleLocks());

    return last_;
}

CheckItem PreflightChecker::checkRootLayout() const {
    CheckItem c;
    c.id = "root.layout";
    c.title = "便携包目录结构完整";

    if (!hs::Paths::isValid()) {
        c.level = CheckLevel::Fail;
        c.detail = "未定位到合法的 Hermes 根目录 (需同时包含 data/、hermes-agent/ 与 venv|python/)";
        c.fixAction = "hint:请把 AgentDock.exe 放在便携包根目录, 或在设置页指定根目录";
        return c;
    }

    c.level = CheckLevel::Pass;
    c.detail = "Hermes 根目录: " + hs::Paths::root().string();
    return c;
}

CheckItem PreflightChecker::checkRuntime() const {
    CheckItem c;
    c.id = "runtime.exe";
    c.title = "运行时可执行文件齐备";

    std::vector<std::string> missing;
    if (hs::Paths::hermesExe().empty())      missing.push_back("hermes.exe");
    if (hs::Paths::nodeExe().empty())        missing.push_back("node.exe");
    if (hs::Paths::venvPython().empty() &&
        hs::Paths::portablePython().empty()) missing.push_back("python.exe");

    if (!missing.empty()) {
        c.level = CheckLevel::Fail;
        c.detail = "缺少: " + [&] {
            std::string s;
            for (size_t i = 0; i < missing.size(); ++i) {
                if (i) s += ", ";
                s += missing[i];
            }
            return s;
        }();
        c.fixAction = "hint:便携包不完整, 请重新下载完整发行包";
        return c;
    }

    c.level = CheckLevel::Pass;
    c.detail = "hermes.exe / node.exe / python.exe 均已就位";
    return c;
}

CheckItem PreflightChecker::checkConfigFiles() const {
    CheckItem c;
    c.id = "config.files";
    c.title = "内核配置文件状态";

    bool hasEnv  = fileExists(hs::Paths::envFile());
    bool hasYaml = fileExists(hs::Paths::configFile());

    if (!hasEnv || !hasYaml) {
        c.level = CheckLevel::Warn;
        c.detail = std::string(hasEnv ? "" : ".env 缺失 ") + (hasYaml ? "" : "config.yaml 缺失")
                 + " — 首次启动时会自动渲染";
        c.fixable = true;
        c.fixAction = "auto:render";
        return c;
    }
    c.level = CheckLevel::Pass;
    c.detail = ".env 与 config.yaml 均已生成";
    return c;
}

CheckItem PreflightChecker::checkPorts() const {
    CheckItem c;
    c.id = "net.ports";
    c.title = "端口可用性";

    int port = ctx_.runtime.webUiPort;
    auto occupant = net::pidByPort(port);
    if (occupant) {
        std::string name = net::processName(*occupant);
        // 若 Hermes 正在运行, 端口被自己占用是正常状态, 不报 warn、不提供停止修复
        if (ctx_.hermesRunning) {
            c.level = CheckLevel::Pass;
            c.detail = "端口 " + std::to_string(port) + " 由当前运行的 Hermes 占用 (pid "
                     + std::to_string(*occupant) + "), 状态正常";
            return c;
        }
        // 端口被占不一定是问题: 可能是上次没关干净的 web-ui
        c.level = CheckLevel::Warn;
        c.detail = "端口 " + std::to_string(port) + " 已被占用 (pid " + std::to_string(*occupant)
                 + (name.empty() ? "" : ", " + name) + "), 可直接连接或先停止它";
        c.fixable = true;
        c.fixAction = "auto:stop-port:" + std::to_string(port);
        return c;
    }

    // 17520 是旧配置中心的端口, 退役后不应再有进程
    auto legacy = net::pidByPort(17520);
    if (legacy) {
        c.level = CheckLevel::Warn;
        c.detail = "检测到旧配置中心仍在运行 (pid " + std::to_string(*legacy) + ", :17520), 建议停止";
        c.fixable = true;
        c.fixAction = "auto:stop-port:17520";
        return c;
    }

    c.level = CheckLevel::Pass;
    c.detail = "端口 " + std::to_string(port) + " 空闲";
    return c;
}

CheckItem PreflightChecker::checkEditablePaths() const {
    CheckItem c;
    c.id = "py.editable";
    c.title = "Python editable 安装路径有效";

    if (editablePathsNeedFix()) {
        c.level = CheckLevel::Warn;
        c.detail = "editable 安装仍指向旧的绝对路径, 会导致 No module named 'hermes_cli'";
        c.fixable = true;
        c.fixAction = "auto:fix-editable";
        return c;
    }
    c.level = CheckLevel::Pass;
    c.detail = "editable 路径与当前根目录一致";
    return c;
}

CheckItem PreflightChecker::checkStaleLocks() const {
    CheckItem c;
    c.id = "lock.stale";
    c.title = "无残留进程锁";

    auto pid = proc::pidFromFile(hs::Paths::gatewayPidFile());
    if (pid && !proc::isRunning(*pid)) {
        c.level = CheckLevel::Warn;
        c.detail = "gateway.pid 指向已退出的进程 (pid " + std::to_string(*pid) + "), 残留锁文件";
        c.fixable = true;
        c.fixAction = "auto:clear-locks";
        return c;
    }
    c.level = CheckLevel::Pass;
    c.detail = pid ? ("Hermes gateway 正在运行 (pid " + std::to_string(*pid) + ")") : "无运行中的 gateway";
    return c;
}

std::string PreflightChecker::applyFix(const std::string& action) {
    if (action.rfind("auto:stop-port:", 0) == 0) {
        int port = std::stoi(action.substr(std::string("auto:stop-port:").size()));
        auto pid = net::pidByPort(port);
        if (!pid) return "端口 " + std::to_string(port) + " 已无占用";
        bool ok = proc::gracefulStop(*pid, 3000);
        return ok ? ("已停止占用端口 " + std::to_string(port) + " 的进程 (pid " + std::to_string(*pid) + ")")
                  : ("停止失败, 请手动结束 pid " + std::to_string(*pid));
    }

    if (action == "auto:clear-locks") {
        std::error_code ec;
        fs::remove(hs::Paths::gatewayPidFile(), ec);
        fs::path st = hs::Paths::dataDir() / "gateway_state.json";
        fs::remove(st, ec);
        return "已清理残留的 gateway 锁文件";
    }

    if (action == "auto:fix-editable") {
        // 便携包被移动后 editable install 的绝对路径失效, 官方修复脚本可幂等修复
        fs::path script = hs::Paths::root() / "lib" / "fix_editable_paths.py";
        std::error_code ec;
        if (!fs::exists(script, ec)) {
            return "未找到 lib/fix_editable_paths.py, 请检查便携包完整性";
        }
        fs::path py = hs::Paths::venvPython();
        if (py.empty()) py = hs::Paths::portablePython();
        if (py.empty()) return "未找到 python.exe, 无法修复";

        proc::LaunchOptions opt;
        opt.exe = py;
        opt.args = {script.string()};
        opt.workingDir = hs::Paths::root();
        opt.hidden = true;
        std::string runErr;
        int code = proc::runAndWait(opt, 60000, &runErr);
        if (code != 0) return "修复脚本执行失败: " + runErr;
        return "已修复 editable 安装路径";
    }

    return "无法自动处理: " + action;
}

bool PreflightChecker::canLaunch() const {
    for (const auto& c : last_) {
        if (c.level == CheckLevel::Fail) return false;
    }
    return true;
}

} // namespace hs
