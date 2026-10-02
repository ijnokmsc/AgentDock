// HermesProc.cpp —— Hermes 进程壳 (差异点实现, 相位机在 WebAgentProc)
#include "HermesProc.h"
#include "Ui.h"

namespace hs {

bool HermesProc::startImpl(std::wstring& err) {
    // 启动模式与端口来自 settings (Bridge/settings 页维护)
    auto mode = g_app.settings.runtime.startMode;
    auto r = launcher_.start(mode, port_);
    if (!r.ok) { err = Utf8ToWide(r.message); return false; }
    return true;
}

}  // namespace hs
