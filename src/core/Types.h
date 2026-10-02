#pragma once
// 领域模型定义。Core 层不依赖 Qt，一律用 std::string / std::optional。
// JSON 序列化交给 nlohmann/json 的宏，保证存储格式与代码结构单一来源。

#include <string>
#include <vector>
#include <optional>
#include <cstdint>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace hs {

// ---------------------------------------------------------------------------
// 错误处理：不用异常做流程控制，统一返回 Result<T>
// ---------------------------------------------------------------------------
struct Error {
    int         code = 0;
    std::string message;
    std::string detail;
};

template <class T>
struct Result {
    std::optional<T> value;
    std::optional<Error> error;

    bool ok() const { return value.has_value(); }
    T& operator*() { return *value; }
    const T& operator*() const { return *value; }

    static Result<T> ok(T v) { Result<T> r; r.value = std::move(v); return r; }
    static Result<T> fail(int code, std::string msg, std::string detail = {}) {
        Result<T> r; r.error = Error{code, std::move(msg), std::move(detail)}; return r;
    }
};

using VoidResult = Result<bool>;
inline VoidResult okVoid() { return Result<bool>::ok(true); }
inline VoidResult failVoid(int code, std::string msg, std::string detail = {}) {
    return Result<bool>::fail(code, std::move(msg), std::move(detail));
}

// ---------------------------------------------------------------------------
// 枚举
// ---------------------------------------------------------------------------
enum class ProviderKind { OpenAI, Anthropic, OpenAICompatible, Ollama };
enum class StartMode    { Cli, WebUI, Desktop };
enum class CheckLevel   { Pass, Warn, Fail };
enum class HermesState  { Stopped, Starting, Running, Stopping, Failed };

enum class ComponentId {
    Kernel,          // hermes-agent 源码树 (官方布局 = <root>/python, 跳过 base/ venv/)
    WebUI,           // <root>/webui (或旧布局 node/node_modules/hermes-web-ui)
    PythonRuntime,   // <root>/python (解释器 base/)
    NodeRuntime,     // <root>/node
    Launcher         // AgentDock.exe 自身
};

inline std::string toString(ComponentId id) {
    switch (id) {
        case ComponentId::Kernel:        return "kernel";
        case ComponentId::WebUI:         return "webui";
        case ComponentId::PythonRuntime: return "python";
        case ComponentId::NodeRuntime:   return "node";
        case ComponentId::Launcher:      return "launcher";
    }
    return "unknown";
}

inline std::string toString(StartMode m) {
    switch (m) {
        case StartMode::Cli:     return "cli";
        case StartMode::WebUI:   return "webui";
        case StartMode::Desktop: return "desktop";
    }
    return "cli";
}

inline std::string toString(ProviderKind k) {
    switch (k) {
        case ProviderKind::OpenAI:             return "openai";
        case ProviderKind::Anthropic:          return "anthropic";
        case ProviderKind::OpenAICompatible:   return "openai-compatible";
        case ProviderKind::Ollama:             return "ollama";
    }
    return "openai-compatible";
}

// ---------------------------------------------------------------------------
// Provider / Model
// ---------------------------------------------------------------------------
struct Provider {
    std::string id;
    std::string displayName;
    std::string kind        = "openai-compatible";
    std::string baseUrl;
    std::string baseUrlEnvVar;          // 内核 <X>_BASE_URL 覆盖变量 (原生厂商, 见 ProviderPresets)
    std::string providerName;           // 内核 provider id (HERMES_OVERLAYS); 空=按 id 直用/custom 兜底
    std::string envVar;                 // 渲染进 .env 的变量名
    std::string apiKey;                 // 明文或密文
    bool        encrypted   = false;
    bool        enabled     = true;
    std::string iconName;
};

struct ModelParams {
    std::optional<double> temperature;
    std::optional<double> topP;
    std::optional<int>    maxTokens;
    std::optional<int>    contextWindow;
    std::optional<int>    timeoutSec;
};

struct Model {
    std::string id;                     // 本地唯一：providerId/upstreamId
    std::string providerId;
    std::string upstreamId;             // 发给上游的 model 名
    std::string displayName;
    ModelParams params;
    bool        supportsTools = true;
    bool        enabled       = true;
    bool        isDefault     = false;
    std::string addedAt;
};

struct ModelConfig {
    int                  schemaVersion = 1;
    std::vector<Provider> providers;
    std::vector<Model>    models;
};

// ---------------------------------------------------------------------------
// 运行时与网络设置
// ---------------------------------------------------------------------------
struct RuntimeConfig {
    std::string hermesRoot;
    std::string pythonExe;
    std::string uvExe;   // 保留以兼容既有 settings.json: 官方 runtime 不含 uv, 恒为空
    std::string nodeExe;
    int         webUiPort        = 8648;
    StartMode   startMode        = StartMode::WebUI;
    bool        autoCheckUpdate  = true;
    bool        launchOnStart    = false;
    std::string updateChannel    = "stable";
    int         backupKeepCount  = 3;
};

enum class ProxyMode { System, Direct, Manual };

struct NetSettings {
    ProxyMode   mode            = ProxyMode::System;
    std::string host;
    int         port            = 0;
    bool        socks5          = false;
    std::string username;
    std::string password;
    std::string bypassList;
    int         timeoutSec      = 30;
    int         retry           = 2;
    bool        allowInsecureTls = false;
    std::string githubMirror;
};

// DeepSeek Harness 便携包子目录 (ADR-008): 与 Hermes 核心初始化完全独立
struct DshSettings {
    bool        enabled        = true;      // 概览显示 DSH 卡片
    int         webUiPort      = 3080;      // dsh web --port
    bool        launchOnStart  = false;     // 启动器启动时同时拉起 dsh
    std::string pinnedVersion  = "0.1.5-rc.2"; // 构建锁定的 @deepseek-ai/dsh 版本
    std::string nodeVersion    = "v24.21.0";   // 构建使用的 Node (LTS)
};

struct AppSettings {
    int          schemaVersion = 1;
    RuntimeConfig runtime;
    NetSettings   network;
    DshSettings   dsh;
    bool          masterPasswordEnabled = false;
    std::string   locale = "zh-CN";
};

// ---------------------------------------------------------------------------
// 体检项
// ---------------------------------------------------------------------------
struct CheckItem {
    std::string id;
    std::string title;
    CheckLevel  level = CheckLevel::Pass;
    std::string detail;
    bool        fixable   = false;
    std::string fixAction;      // "auto:<id>" | "goto:models" | "hint"
};

// ---------------------------------------------------------------------------
// 组件状态
// ---------------------------------------------------------------------------
struct ComponentState {
    ComponentId id;
    std::string name;
    std::string currentVersion;
    std::string latestVersion;
    std::string installPath;
    bool        updateAvailable = false;
    bool        supported       = true;      // 例如 Desktop 缺失时为 false
    std::string note;
    std::int64_t lastCheckTs = 0;
};

// ---------------------------------------------------------------------------
// JSON 适配
// ---------------------------------------------------------------------------
// Provider 手写 JSON 适配 (不用宏): from_json 用 value() 带默认值,
// 兼容缺 providerName/baseUrlEnvVar 的旧 models.json (宏的 .at() 会抛异常)。
inline void to_json(json& j, const Provider& p) {
    j = json{{"id", p.id}, {"displayName", p.displayName}, {"kind", p.kind},
             {"baseUrl", p.baseUrl}, {"baseUrlEnvVar", p.baseUrlEnvVar},
             {"providerName", p.providerName}, {"envVar", p.envVar}, {"apiKey", p.apiKey},
             {"encrypted", p.encrypted}, {"enabled", p.enabled}, {"iconName", p.iconName}};
}
inline void from_json(const json& j, Provider& p) {
    p.id            = j.value("id", "");
    p.displayName   = j.value("displayName", "");
    p.kind          = j.value("kind", "openai-compatible");
    p.baseUrl       = j.value("baseUrl", "");
    p.baseUrlEnvVar = j.value("baseUrlEnvVar", "");
    p.providerName  = j.value("providerName", "");
    p.envVar        = j.value("envVar", "");
    p.apiKey        = j.value("apiKey", "");
    p.encrypted     = j.value("encrypted", false);
    p.enabled       = j.value("enabled", true);
    p.iconName      = j.value("iconName", "");
}

// ModelParams 含 std::optional 成员, 不能用 NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE 宏
// (宏生成的 to_json 无法把 optional<T> 赋给 json)。这里手写: 有值才写, 缺省即未设置。
inline void to_json(json& j, const ModelParams& p) {
    j = json::object();
    if (p.temperature)   j["temperature"]   = *p.temperature;
    if (p.topP)          j["topP"]          = *p.topP;
    if (p.maxTokens)     j["maxTokens"]     = *p.maxTokens;
    if (p.contextWindow) j["contextWindow"] = *p.contextWindow;
    if (p.timeoutSec)    j["timeoutSec"]    = *p.timeoutSec;
}

inline void from_json(const json& j, ModelParams& p) {
    auto getOpt = [&j](const char* key) -> std::optional<json> {
        if (!j.contains(key) || j.at(key).is_null()) return std::nullopt;
        return j.at(key);
    };
    if (auto v = getOpt("temperature"))   p.temperature   = v->get<double>();
    if (auto v = getOpt("topP"))          p.topP          = v->get<double>();
    if (auto v = getOpt("maxTokens"))     p.maxTokens     = v->get<int>();
    if (auto v = getOpt("contextWindow")) p.contextWindow = v->get<int>();
    if (auto v = getOpt("timeoutSec"))    p.timeoutSec    = v->get<int>();
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Model,
    id, providerId, upstreamId, displayName, params,
    supportsTools, enabled, isDefault, addedAt)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ModelConfig, schemaVersion, providers, models)

// WITH_DEFAULT: settings.json 手工编辑缺字段时用默认值, 不再整单抛异常重置 (军规 #12 配套)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(RuntimeConfig,
    hermesRoot, pythonExe, uvExe, nodeExe, webUiPort,
    startMode, autoCheckUpdate, launchOnStart, updateChannel, backupKeepCount)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(NetSettings,
    mode, host, port, socks5, username, password, bypassList,
    timeoutSec, retry, allowInsecureTls, githubMirror)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(DshSettings,
    enabled, webUiPort, launchOnStart, pinnedVersion, nodeVersion)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(AppSettings,
    schemaVersion, runtime, network, dsh, masterPasswordEnabled, locale)

} // namespace hs
