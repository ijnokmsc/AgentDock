// DshProc.h —— DeepSeek Harness 进程管理薄壳 (ADR-008, WebAgentProc 子类)
//
// 与 HermesProc 共用同一份相位状态机; 差异点:
//   - startImpl 委托 hs::dsh::start (内部捕获 stdout token 横幅)
//   - pageUrl 覆写为带 token 的 URL (每 boot 变化)
//   - 无 hs::Launcher / coreState —— DSH 与 Hermes 核心 (coreReady) 完全独立,
//     PendingBuild 模式下照样可用。
#pragma once
#include <windows.h>
#include <string>
#include "WebAgentProc.h"

namespace hs {

class DshProc : public WebAgentProc {
public:
    void Init(const std::wstring& root, int port);

    std::wstring TokenUrl() const;                   // 透传 hs::dsh::tokenUrl()
    // OpenBrowser 用基类实现 (经虚函数 pageUrl 自动带上 token)

protected:
    bool startImpl(std::wstring& err) override;
    void stopImpl() override;
    std::wstring pageUrl() const override;

private:
    std::wstring root_;
};

}  // namespace hs
