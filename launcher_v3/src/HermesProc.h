// HermesProc.h —— Hermes 进程管理 (WebAgentProc 子类)
//
// 通用相位机/启停/查询在 WebAgentProc; 本类只接核心层 hs::Launcher:
// startImpl 委托 Launcher (veto 检查由调用方先做), coreState 提供
// Launcher 状态兜底 (Starting/Stopping), 打开浏览器用裸端口 URL。
#pragma once
#include <windows.h>
#include <string>
#include "WebAgentProc.h"
#include "core/Launcher.h"

namespace hs {

class HermesProc : public WebAgentProc {
public:
    void Init(const std::wstring& root, int port) { root_ = root; Bind(port); }

    hs::Launcher& L() { return launcher_; }
    const std::wstring& Root() const { return root_; }

protected:
    bool startImpl(std::wstring& err) override;
    void stopImpl() override { launcher_.stop(); }
    std::optional<HermesState> coreState() const override { return launcher_.state(); }

private:
    std::wstring root_;
    hs::Launcher launcher_;
};

}  // namespace hs
