#include "ProviderPresets.h"

namespace hs {

const std::vector<ProviderPreset>& providerPresets() {
    static const std::vector<ProviderPreset> kPresets = {
        // ---------------- 国内主流 ----------------
        {"deepseek", "DeepSeek", "国内主流", "deepseek", "openai-compatible",
         "https://api.deepseek.com/v1", "DEEPSEEK_BASE_URL", "DEEPSEEK_API_KEY", true,
         "https://platform.deepseek.com/api_keys",
         "深度求索官方 OpenAI 兼容端点",
         {{"deepseek-chat", "DeepSeek Chat"}, {"deepseek-reasoner", "DeepSeek Reasoner"}}},

        {"glm", "智谱 GLM", "国内主流", "zai", "openai-compatible",
         "https://open.bigmodel.cn/api/paas/v4", "GLM_BASE_URL", "GLM_API_KEY", true,
         "https://open.bigmodel.cn/usercenter/apikeys",
         "智谱 BigModel 开放平台 (内核 provider id: zai)",
         {{"glm-4.6", "GLM-4.6"}, {"glm-4.5", "GLM-4.5"}, {"glm-4.5-air", "GLM-4.5-Air"}}},

        {"qwen", "通义千问 Qwen", "国内主流", "alibaba", "openai-compatible",
         "https://dashscope.aliyuncs.com/compatible-mode/v1", "DASHSCOPE_BASE_URL", "DASHSCOPE_API_KEY", true,
         "https://bailian.console.aliyun.com/",
         "阿里云百炼 DashScope 兼容模式 (内核 provider id: alibaba)",
         {{"qwen3-max", "Qwen3 Max"}, {"qwen-plus", "Qwen Plus"}, {"qwen3-coder-plus", "Qwen3 Coder Plus"}}},

        {"kimi", "Kimi (月之暗面)", "国内主流", "kimi-for-coding", "openai-compatible",
         "https://api.moonshot.ai/v1", "KIMI_BASE_URL", "KIMI_API_KEY", true,
         "https://platform.moonshot.cn/console/api-keys",
         "Moonshot 开放平台 (内核 provider id: kimi-for-coding)",
         {{"kimi-k2-0905-preview", "Kimi K2 0905"}, {"kimi-k2-turbo-preview", "Kimi K2 Turbo"}}},

        {"doubao", "豆包 (火山方舟)", "国内主流", "", "openai-compatible",
         "https://ark.cn-beijing.volces.com/api/v3", "", "VOLCES_API_KEY", true,
         "https://console.volcengine.com/ark",
         "火山方舟 OpenAI 兼容端点; 也支持直接填 ep-xxx 接入点 ID 作模型名",
         {{"doubao-seed-1-6", "Doubao Seed 1.6"}}},

        {"siliconflow", "硅基流动", "国内主流", "", "openai-compatible",
         "https://api.siliconflow.cn/v1", "", "SILICONFLOW_API_KEY", true,
         "https://cloud.siliconflow.cn/account/ak",
         "聚合开源模型的国内推理平台",
         {{"deepseek-ai/DeepSeek-V3.2", "DeepSeek V3.2"}, {"Qwen/Qwen3-32B", "Qwen3 32B"}}},

        // ---------------- 国际主流 ----------------
        {"openai", "OpenAI", "国际主流", "", "openai-compatible",
         "https://api.openai.com/v1", "", "OPENAI_API_KEY", true,
         "https://platform.openai.com/api-keys",
         "OpenAI 官方端点 (Key 由内核按 openai.com host 门控, 不会外泄)",
         {{"gpt-5", "GPT-5"}, {"gpt-5-mini", "GPT-5 Mini"}, {"gpt-4o", "GPT-4o"}}},

        {"anthropic", "Anthropic Claude", "国际主流", "anthropic", "anthropic",
         "", "", "ANTHROPIC_API_KEY", true,
         "https://console.anthropic.com/settings/keys",
         "Claude 原生 Messages 协议 (端点由内核内置)",
         {{"claude-sonnet-4-5", "Claude Sonnet 4.5"},
          {"claude-haiku-4-5", "Claude Haiku 4.5"},
          {"claude-opus-4-1", "Claude Opus 4.1"}}},

        {"gemini", "Google Gemini", "国际主流", "google", "openai-compatible",
         "", "", "GEMINI_API_KEY", true,
         "https://aistudio.google.com/apikey",
         "Gemini 原生接入 (内核 provider id: google)",
         {{"gemini-2.5-pro", "Gemini 2.5 Pro"}, {"gemini-2.5-flash", "Gemini 2.5 Flash"}}},

        {"openrouter", "OpenRouter", "国际主流", "openrouter", "openai-compatible",
         "", "", "OPENROUTER_API_KEY", true,
         "https://openrouter.ai/keys",
         "多厂商聚合网关 (一个 Key 调用数百家模型)",
         {{"openrouter/auto", "Auto (自动选模型)"}}},

        // ---------------- 本地自托管 ----------------
        {"ollama", "Ollama (本地)", "本地自托管", "", "ollama",
         "http://127.0.0.1:11434/v1", "", "", false,
         "",
         "本地无需 Key; 模型名以 ollama list 已拉取的为准",
         {{"llama3.1", "Llama 3.1"}, {"qwen3", "Qwen3"}}},

        {"lmstudio", "LM Studio (本地)", "本地自托管", "lmstudio", "openai-compatible",
         "http://127.0.0.1:1234/v1", "LM_BASE_URL", "", false,
         "",
         "LM Studio 本地服务端 (内核 provider id: lmstudio)",
         {}},

        {"vllm", "vLLM (自托管)", "本地自托管", "", "openai-compatible",
         "http://127.0.0.1:8000/v1", "", "", false,
         "",
         "vLLM / llama.cpp 等本地 OpenAI 兼容服务; 按实际部署改地址",
         {}},

        // ---------------- 通用 ----------------
        {"custom-blank", "通用自定义 (OpenAI 兼容)", "通用", "", "openai-compatible",
         "", "", "", true,
         "",
         "空白模板: 手填显示名 / Base URL / Key 变量名; Key 变量建议按 <厂商>_API_KEY 命名 "
         "(内核按 Base URL 的域名段匹配 Key, 如 api.moonshot.cn → MOONSHOT_API_KEY)",
         {}},
    };
    return kPresets;
}

std::string resolveKernelProviderName(const std::string& id, const std::string& providerName) {
    if (!providerName.empty()) return providerName;
    if (id.empty() || id == "custom") return "custom";
    // 内核已知 provider id (HERMES_OVERLAYS 与 models.dev 目录中常用的子集)。
    // 命中则直接作 model.provider, 未知名字一律落 custom (内核对未知 id 报 invalid_provider)。
    static const char* kKnown[] = {
        "deepseek", "zai", "kimi-for-coding", "alibaba", "openai-api", "openrouter",
        "anthropic", "google", "lmstudio", "ollama-cloud", "xai", "xai-oauth",
        "minimax", "minimax-cn", "stepfun", "qwen-oauth", "nous", "bedrock",
    };
    for (const char* k : kKnown)
        if (id == k) return id;
    return "custom";
}

}  // namespace hs
