// DshProc.cpp —— dsh 进程壳差异点实现 (相位机在 WebAgentProc)
#include "DshProc.h"
#include "Ui.h"
#include "core/DshRuntime.h"

#include <shellapi.h>

namespace hs {

void DshProc::Init(const std::wstring& root, int port) {
    root_ = root;
    Bind(port);
    hs::dsh::setRoot(fs::path(root));
}

bool DshProc::startImpl(std::wstring& err) {
    std::string serr;
    auto r = hs::dsh::start(port_, &serr);
    if (!r.ok) { err = Utf8ToWide(serr); return false; }
    return true;
}

void DshProc::stopImpl() { hs::dsh::stop(port_); }

std::wstring DshProc::pageUrl() const { return TokenUrl(); }

std::wstring DshProc::TokenUrl() const { return hs::dsh::tokenUrl(); }

}  // namespace hs
