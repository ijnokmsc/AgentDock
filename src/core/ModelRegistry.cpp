#include "ModelRegistry.h"

#include "../storage/JsonStore.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <regex>

namespace {

std::string nowIso() {
    auto t = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(t);
    std::tm tm{};
    localtime_s(&tm, &tt);
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
    return os.str();
}

// 从 "DEEPSEEK_API_KEY" 推断 "deepseek"
std::string providerIdFromEnvVar(const std::string& var) {
    static const std::regex re(R"(^(.+?)_API_KEY$)");
    std::smatch m;
    if (std::regex_match(var, m, re)) {
        std::string id = m[1].str();
        std::transform(id.begin(), id.end(), id.begin(), [](unsigned char c) { return (char)::tolower(c); });
        return id;
    }
    return {};
}

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)::toupper(c); });
    return s;
}

// 极简 .env 解析: KEY=VALUE, 忽略 # 注释
std::vector<std::pair<std::string, std::string>> parseEnvFile(const fs::path& p) {
    std::vector<std::pair<std::string, std::string>> out;
    std::ifstream in(p);
    if (!in) return out;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        // 去掉成对引号
        if (v.size() >= 2 && v.front() == v.back() && (v.front() == '"' || v.front() == '\'')) {
            v = v.substr(1, v.size() - 2);
        }
        out.emplace_back(k, v);
    }
    return out;
}

// 从 config.yaml 抽取 "key: value" 的顶层/二级简单字段
std::optional<std::string> yamlSimpleField(const std::string& text, const std::string& parent, const std::string& key) {
    std::istringstream in(text);
    std::string line;
    bool inSection = parent.empty();
    std::regex secRe(R"(^([A-Za-z_][\w-]*):\s*$)");
    std::regex kvRe(R"(^\s+([A-Za-z_][\w-]*):\s*(.+?)\s*$)");
    std::smatch m;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (std::regex_match(line, m, secRe)) {
            inSection = parent.empty() ? true : (m[1].str() == parent);
            continue;
        }
        if (inSection && std::regex_match(line, m, kvRe)) {
            if (m[1].str() == key) return m[2].str();
        }
    }
    return std::nullopt;
}

// 抽取 config.yaml 里 mcp_servers: 起头的整段 (到下一个顶层键为止)
std::string extractMcpSection(const std::string& text) {
    std::istringstream in(text);
    std::string line, out;
    bool collecting = false;
    std::regex topRe(R"(^([A-Za-z_][\w-]*):\s*$)");
    while (std::getline(in, line)) {
        std::string raw = line;
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        std::smatch m;
        if (std::regex_match(raw, m, topRe)) {
            if (m[1].str() == "mcp_servers") { collecting = true; out += raw + "\n"; continue; }
            if (collecting) break;   // 遇到下一个顶层键, 结束
            continue;
        }
        if (collecting) out += raw + "\n";
    }
    return out;
}

void trim(std::string& s) {
    auto l = s.find_first_not_of(" \t");
    auto r = s.find_last_not_of(" \t");
    if (l == std::string::npos) { s.clear(); return; }
    s = s.substr(l, r - l + 1);
}
} // namespace

namespace hs {

ModelRegistry::ModelRegistry(fs::path file) : file_(std::move(file)) {}

VoidResult ModelRegistry::load() {
    auto j = JsonStore::load(file_);
    if (!j) {
        cfg_ = ModelConfig{};
        return okVoid();   // 文件不存在不算错误, 交给 migrate 处理
    }
    try {
        cfg_ = j->get<ModelConfig>();
    } catch (const std::exception& e) {
        return failVoid(10, "models.json 解析失败", e.what());
    }
    return okVoid();
}

VoidResult ModelRegistry::save() const {
    json j = cfg_;
    return JsonStore::save(file_, j);
}

std::optional<Provider> ModelRegistry::provider(const std::string& id) const {
    for (const auto& p : cfg_.providers) if (p.id == id) return p;
    return std::nullopt;
}

VoidResult ModelRegistry::addProvider(const Provider& p) {
    if (p.id.empty()) return failVoid(20, "Provider id 不能为空");
    if (provider(p.id)) return failVoid(21, "Provider 已存在", p.id);
    cfg_.providers.push_back(p);
    return save();
}

VoidResult ModelRegistry::updateProvider(const Provider& p) {
    for (auto& x : cfg_.providers) {
        if (x.id == p.id) {
            std::string oldKey = x.apiKey;
            x = p;
            // UI 不回传密钥时保留原值
            if (p.apiKey.empty()) x.apiKey = oldKey;
            return save();
        }
    }
    return failVoid(22, "Provider 不存在", p.id);
}

VoidResult ModelRegistry::removeProvider(const std::string& id) {
    auto before = cfg_.providers.size();
    cfg_.providers.erase(
        std::remove_if(cfg_.providers.begin(), cfg_.providers.end(),
                       [&](const Provider& p) { return p.id == id; }),
        cfg_.providers.end());
    if (cfg_.providers.size() == before) return failVoid(23, "Provider 不存在", id);

    // 连带删除该 provider 下的模型
    cfg_.models.erase(
        std::remove_if(cfg_.models.begin(), cfg_.models.end(),
                       [&](const Model& m) { return m.providerId == id; }),
        cfg_.models.end());
    return save();
}

std::vector<Model> ModelRegistry::modelsOf(const std::string& providerId) const {
    std::vector<Model> out;
    for (const auto& m : cfg_.models) if (m.providerId == providerId) out.push_back(m);
    return out;
}

std::optional<Model> ModelRegistry::model(const std::string& id) const {
    for (const auto& m : cfg_.models) if (m.id == id) return m;
    return std::nullopt;
}

std::optional<Model> ModelRegistry::defaultModel() const {
    for (const auto& m : cfg_.models) if (m.isDefault && m.enabled) return m;
    return std::nullopt;
}

VoidResult ModelRegistry::addModel(const Model& m) {
    if (m.id.empty()) return failVoid(30, "Model id 不能为空");
    if (model(m.id)) return failVoid(31, "模型已存在", m.id);
    Model copy = m;
    if (copy.addedAt.empty()) copy.addedAt = nowIso();
    // 第一个模型自动成为默认
    if (copy.isDefault || cfg_.models.empty()) {
        for (auto& x : cfg_.models) x.isDefault = false;
        copy.isDefault = true;
    }
    cfg_.models.push_back(copy);
    return save();
}

VoidResult ModelRegistry::updateModel(const Model& m) {
    for (auto& x : cfg_.models) {
        if (x.id == m.id) {
            bool wasDefault = x.isDefault;
            x = m;
            if (m.isDefault || wasDefault) {
                for (auto& y : cfg_.models) y.isDefault = (y.id == m.id);
            }
            return save();
        }
    }
    return failVoid(32, "模型不存在", m.id);
}

VoidResult ModelRegistry::removeModel(const std::string& id) {
    auto before = cfg_.models.size();
    cfg_.models.erase(
        std::remove_if(cfg_.models.begin(), cfg_.models.end(),
                       [&](const Model& m) { return m.id == id; }),
        cfg_.models.end());
    if (cfg_.models.size() == before) return failVoid(33, "模型不存在", id);

    // 默认模型被删则自动补一个
    if (!defaultModel() && !cfg_.models.empty()) cfg_.models.front().isDefault = true;
    return save();
}

VoidResult ModelRegistry::setDefault(const std::string& modelId) {
    if (!model(modelId)) return failVoid(34, "模型不存在", modelId);
    for (auto& m : cfg_.models) m.isDefault = (m.id == modelId);
    return save();
}

ModelRegistry::MigrationResult ModelRegistry::migrateFromLegacy(const fs::path& envFile,
                                                                const fs::path& configFile,
                                                                const fs::path& templateOut) {
    MigrationResult rep;

    // ---- 1) .env -> providers ----
    std::string customBaseUrl;
    for (const auto& kv : parseEnvFile(envFile)) {
        std::string key = kv.first, val = kv.second;
        trim(val);
        if (val.empty()) continue;

        if (key == "CUSTOM_BASE_URL") { customBaseUrl = val; continue; }

        std::string pid = providerIdFromEnvVar(key);
        if (pid.empty()) continue;

        Provider p;
        p.id        = pid;
        p.displayName = pid;
        p.envVar    = key;
        p.apiKey    = val;
        p.enabled   = true;
        p.kind      = toString(ProviderKind::OpenAICompatible);
        p.baseUrl   = customBaseUrl;   // 只有 custom 需要, 下面会修正
        cfg_.providers.push_back(p);
        ++rep.providersImported;
    }
    for (auto& p : cfg_.providers) {
        if (p.id != "custom") p.baseUrl.clear();   // 非 custom 让内核用默认 endpoint
    }

    // ---- 2) config.yaml -> 默认模型 + mcp_servers 模板 ----
    std::string cfgText;
    {
        std::ifstream in(configFile);
        if (in) { std::ostringstream ss; ss << in.rdbuf(); cfgText = ss.str(); }
    }

    if (!cfgText.empty()) {
        auto def  = yamlSimpleField(cfgText, "model", "default");
        auto prov = yamlSimpleField(cfgText, "model", "provider");
        auto up   = yamlSimpleField(cfgText, "model", "upstream_id");

        if (def && !def->empty()) {
            Model m;
            m.displayName = *def;
            m.upstreamId  = up && !up->empty() ? *up : *def;
            m.providerId  = prov && !prov->empty() ? *prov
                                                   : (cfg_.providers.empty() ? std::string("custom")
                                                                             : cfg_.providers.front().id);
            m.id          = m.providerId + "/" + m.upstreamId;
            m.isDefault   = true;
            m.enabled     = true;
            m.addedAt     = nowIso();
            cfg_.models.push_back(m);
            ++rep.modelsImported;
        } else {
            rep.warnings.push_back("config.yaml 中未找到 model.default, 未导入模型");
        }

        // mcp_servers 段落落成模板
        std::string mcp = extractMcpSection(cfgText);
        if (!mcp.empty()) {
            JsonStore::ensureParent(templateOut);
            std::ofstream out(templateOut, std::ios::binary | std::ios::trunc);
            if (out) {
                out << "# 由 AgentDock 从原 config.yaml 抽取, 每次渲染时原样附加\n";
                out << mcp;
                rep.configTemplateCreated = true;
            }
        } else {
            rep.warnings.push_back("config.yaml 中未找到 mcp_servers 段, 未生成模板");
        }
    }

    if (cfg_.providers.empty()) {
        rep.warnings.push_back(".env 中未解析到任何 *_API_KEY, 请手动添加 Provider");
    }

    (void)save();
    return rep;
}

std::vector<CheckItem> ModelRegistry::validate() const {
    std::vector<CheckItem> out;

    int enabledProviders = 0;
    for (const auto& p : cfg_.providers) if (p.enabled) ++enabledProviders;

    CheckItem c1;
    c1.id = "models.provider";
    c1.title = "至少一个可用的 Provider";
    if (enabledProviders == 0) {
        c1.level = CheckLevel::Fail;
        c1.detail = "尚未配置任何 Provider, Hermes 无法调用模型";
        c1.fixable = true;
        c1.fixAction = "goto:models";
    } else {
        c1.level = CheckLevel::Pass;
        c1.detail = std::to_string(enabledProviders) + " 个 Provider 已启用";
    }
    out.push_back(c1);

    // Key 非空
    bool anyEmptyKey = false;
    for (const auto& p : cfg_.providers) {
        if (p.enabled && p.apiKey.empty()) anyEmptyKey = true;
    }
    CheckItem c2;
    c2.id = "models.apikey";
    c2.title = "已启用 Provider 的 API Key 非空";
    if (anyEmptyKey) {
        c2.level = CheckLevel::Fail;
        c2.detail = "存在已启用但 API Key 为空的 Provider";
        c2.fixable = true;
        c2.fixAction = "goto:models";
    } else {
        c2.level = CheckLevel::Pass;
        c2.detail = "全部已启用 Provider 均有 Key";
    }
    out.push_back(c2);

    // 默认模型唯一
    int defaults = 0;
    for (const auto& m : cfg_.models) if (m.isDefault) ++defaults;

    CheckItem c3;
    c3.id = "models.default";
    c3.title = "默认模型唯一且有效";
    if (defaults == 0) {
        c3.level = CheckLevel::Fail;
        c3.detail = "未设置默认模型";
        c3.fixable = true;
        c3.fixAction = "auto:pick-default";
    } else if (defaults > 1) {
        c3.level = CheckLevel::Fail;
        c3.detail = "存在 " + std::to_string(defaults) + " 个默认模型";
        c3.fixable = true;
        c3.fixAction = "auto:pick-default";
    } else {
        c3.level = CheckLevel::Pass;
        auto d = defaultModel();
        c3.detail = d ? ("默认模型: " + d->displayName) : "默认模型不可用";
    }
    out.push_back(c3);

    return out;
}

} // namespace hs
