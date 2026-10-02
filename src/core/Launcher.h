#pragma once

#include "Types.h"
#include "../app/Paths.h"
#include <optional>
#include <vector>

namespace hs {

// Hermes 三种运行模式的拉起与停止。
//
//   Cli     : <venv>\Scripts\python.exe -m hermes_cli.main  —— 独立控制台窗口
//   WebUI   : node webui\dist\server\index.js start <port>  —— 后台进程
//   Desktop : desktop\**\*.exe —— Electron 独立窗口 (并非每个包都有)
//
// 路径全部走 Paths 的布局感知解析, 官方 runtime 布局与旧第三方布局都能跑。
//
// 停止策略: 优先读 data/gateway.pid 定位内核进程, 端口反查定位 web-ui,
// 再按进程树终止, 最后清理残留锁文件。
class Launcher {
public:
    Launcher();

    struct StartResult {
        bool        ok = false;
        std::string message;
        std::optional<unsigned long> kernelPid;
        std::optional<unsigned long> webUiPid;
        std::optional<unsigned long> desktopPid;
    };

    StartResult start(StartMode mode, int webUiPort = 8648);
    bool        stop();

    HermesState state() const;
    StartMode   mode() const { return mode_; }

    // 当前 web-ui 地址 (用于「打开浏览器」)
    std::string webUiUrl(int port = 8648) const;

    // 该模式在当前包内是否可用 (例如 Desktop 缺运行时)
    bool modeAvailable(StartMode mode) const;
    std::string modeUnavailableReason(StartMode mode) const;

private:
    StartResult startCli();
    StartResult startWebUi(int port);
    StartResult startDesktop();

    std::optional<unsigned long> kernelPid_;
    std::optional<unsigned long> webUiPid_;
    std::optional<unsigned long> desktopPid_;
    int       webUiPort_ = 8648;
    StartMode mode_ = StartMode::WebUI;
};

} // namespace hs
