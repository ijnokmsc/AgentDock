#include "AgentSpec.h"

namespace hs::agents {

const std::vector<AgentDescriptor>& registry() {
    static const std::vector<AgentDescriptor> kAgents = {
        {"hermes", "Hermes", "Hermes 便携包",
         "从 GitHub Release 拉取 runtime + web-ui 组装; 目标目录默认 .\\Hermes (也支持填 . 在便携包根就地组装)",
         ".\\Hermes", 8648, ProbeKind::PortReady,
         "build", "kernel,webui", true},
        {"dsh", "DeepSeek Harness", "DSH 便携包",
         "nodejs.org 拉取 Node + npm 安装 @deepseek-ai/dsh; 产物在 .\\DSH, 存储/缓存全部钉在包内 (ADR-008)",
         ".\\DSH", 3080, ProbeKind::HttpWithToken,
         "build.dsh", "dsh,dshnode", true},
    };
    return kAgents;
}

const AgentDescriptor* find(const std::string& id) {
    for (auto& a : registry())
        if (id == a.id) return &a;
    return nullptr;
}

}  // namespace hs::agents
