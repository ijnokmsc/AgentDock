#pragma once
// WebAgentProc.h —— Agent 进程壳通用基类 (Hermes / DSH 共用)
//
// 相位状态机、端口探活、内存/时长查询、45s 启动判定、停止兜底只有这一份实现;
// 子类只提供差异点:
//   startImpl()  怎么拉起   (Hermes=hs::Launcher / DSH=hs::dsh::start)
//   stopImpl()   怎么停
//   coreState()  可选: 核心层状态兜底 (仅 Hermes 的 Launcher 提供)
//   pageUrl()    可选: 打开浏览器的 URL (DSH 覆写为带 token 的地址)
//
// Phase 状态机:
//   Stopped --Start()--> Starting --端口(+HTTP)就绪--> Running
//   Starting --45s 超时--> Failed (显示"已掉线", 重启置顶)
//   Running --Stop()--> Stopped ; Running 端口消失 --> Failed
//
// 端口监听 ≠ HTTP 就绪 (dsh 监听后 ~5s 才应答): 是否切嵌入页由壳层的
// HTTP 探测 (WM_DSH_READY) 决定, 本类只负责进程相位。
#include <windows.h>
#include <string>
#include <optional>
#include "core/Types.h"
#include "platform/PortScanner.h"

namespace hs {

enum class Phase { Stopped, Starting, Running, Failed };

class WebAgentProc {
public:
    struct Status {
        Phase     phase = Phase::Stopped;
        DWORD     pid = 0;
        ULONGLONG memMB = 0;
        ULONGLONG uptimeSec = 0;
        ULONGLONG startingSec = 0;   // Starting 已持续秒数
    };

    void Bind(int port) { port_ = port; }
    int  Port() const { return port_; }

    // ---- 控制 ----
    bool Start(std::wstring& err) {
        if (IsUp()) return true;
        if (!startImpl(err)) return false;
        startAtMs_ = GetTickCount64();
        starting_ = true;                  // 端口就绪前的 Starting 相位锚点
        return true;
    }
    void Stop() {
        starting_ = false;
        stopImpl();
        // 兜底: 记录的 pid 丢失时按端口反查 + taskkill
        Sleep(300);
        if (DWORD pid = ListenerPid()) killByPid(pid);
    }
    void Restart() {
        Stop();
        for (int i = 0; i < 10 && ListenerPid() != 0; ++i) Sleep(200);
        std::wstring err;
        Start(err);
    }

    // ---- 查询 (轻量, 可 1s 轮询; 内部推进状态机) ----
    Status Query();
    bool   IsUp() { return ListenerPid() != 0; }
    DWORD  ListenerPid() { return net::pidByPort(port_).value_or(0); }
    void   OpenBrowser();

    static constexpr ULONGLONG kStartTimeoutSec = 45;

protected:
    virtual bool startImpl(std::wstring& err) = 0;
    virtual void stopImpl() {}
    // 核心层状态兜底 (仅 Hermes 的 hs::Launcher 提供; DSH 只看端口)
    virtual std::optional<HermesState> coreState() const { return std::nullopt; }
    // 系统浏览器打开的地址 (DSH 覆写为带会话 token 的 URL)
    virtual std::wstring pageUrl() const;

    void killByPid(DWORD pid);

    int       port_ = 0;
    ULONGLONG startAtMs_ = 0;
    bool      starting_ = false;   // Start() 已调用且端口尚未就绪
    Phase     lastPhase_ = Phase::Stopped;
};

std::wstring FormatUptime(ULONGLONG sec);
std::wstring FormatMem(ULONGLONG mb);
const wchar_t* PhaseText(Phase p);
bool PhaseIsWarn(Phase p);
bool PhaseIsBusy(Phase p);
Phase PhaseFromHermes(HermesState s);

}  // namespace hs
