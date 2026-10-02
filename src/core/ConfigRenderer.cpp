#include "ConfigRenderer.h"
#include "ProviderPresets.h"
#include "../storage/JsonStore.h"

#include <fstream>
#include <sstream>
#include <windows.h>

namespace {

bool writeTextAtomic(const fs::path& path, const std::string& text) {
    hs::JsonStore::ensureParent(path);
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << text;
        out.flush();
        if (!out) return false;
    }
    if (!MoveFileExW(tmp.wstring().c_str(), path.wstring().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ec;
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

bool copyToBak(const fs::path& src) {
    std::error_code ec;
    if (!fs::exists(src, ec)) return true;
    fs::path bak = src;
    bak += ".bak";
    fs::copy_file(src, bak, fs::copy_options::overwrite_existing, ec);
    return !ec;
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// 取默认模型 (没有就构造一个空壳, 保证 YAML 结构完整)
hs::Model defaultModelOf(const hs::ModelConfig& cfg) {
    for (const auto& m : cfg.models) {
        if (m.isDefault && m.enabled) return m;
    }
    for (const auto& m : cfg.models) {
        if (m.enabled) return m;
    }
    return cfg.models.empty() ? hs::Model{} : cfg.models.front();
}

// YAML 字符串转义: 含特殊字符就加双引号
std::string yamlScalar(const std::string& raw) {
    bool needQuote = raw.empty() ||
        raw.find_first_of(":#{}[]&*!|>'\"%@`\n") != std::string::npos;
    if (!needQuote) return raw;
    std::string out = "\"";
    for (char c : raw) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    out += "\"";
    return out;
}
} // namespace

namespace hs {

ConfigRenderer::ConfigRenderer(fs::path envOut, fs::path yamlOut, fs::path templateFile)
    : envOut_(std::move(envOut)), yamlOut_(std::move(yamlOut)), templateFile_(std::move(templateFile)) {}

VoidResult ConfigRenderer::renderEnv(const ModelConfig& cfg) const {
    std::ostringstream os;
    os << "# 由 AgentDock 自动生成 - 请勿手工编辑 (改模型请到启动器「模型配置」页)\n";
    os << "# 渲染时间: " << cfg.schemaVersion << " (schema v1)\n\n";

    for (const auto& p : cfg.providers) {
        if (!p.enabled) continue;
        if (p.envVar.empty()) continue;
        os << p.envVar << "=" << p.apiKey << "\n";
    }
    // CUSTOM_BASE_URL: 保留 custom provider 的自定义端点 (旧约定, 兼容既有部署)
    for (const auto& p : cfg.providers) {
        if (p.enabled && !p.baseUrl.empty() && p.id == "custom") {
            os << "CUSTOM_BASE_URL=" << p.baseUrl << "\n";
            break;
        }
    }
    // 原生厂商 base URL 覆盖: 内核按 base_url_env_var 约定读取 (见 ProviderPresets.h 实测注)
    for (const auto& p : cfg.providers) {
        if (p.enabled && !p.baseUrlEnvVar.empty() && !p.baseUrl.empty())
            os << p.baseUrlEnvVar << "=" << p.baseUrl << "\n";
    }

    if (!writeTextAtomic(envOut_, os.str())) {
        return failVoid(40, "写入 .env 失败", envOut_.u8string());
    }
    return okVoid();
}

VoidResult ConfigRenderer::renderConfigYaml(const ModelConfig& cfg,
                                            const RuntimeConfig& rt,
                                            const RenderOptions& opt) const {
    std::ostringstream os;
    os << "# 由 AgentDock 自动生成 - 请勿手工编辑\n";
    os << "# 动态段由启动器渲染, mcp_servers 段来自 config.template.yaml (原样保留)\n\n";

    // ---- 动态段 ----
    (void)rt;
    auto def = defaultModelOf(cfg);
    const hs::Provider* defProv = nullptr;
    for (const auto& p : cfg.providers)
        if (p.id == def.providerId) { defProv = &p; break; }

    // model.provider 必须是内核认识的 provider id (未知 id 内核直接 AuthError):
    // 显式 providerName > 已知内核 id > custom 兜底 (resolveKernelProviderName)。
    std::string pname = defProv ? resolveKernelProviderName(defProv->id, defProv->providerName)
                                : resolveKernelProviderName(def.providerId, "");

    os << "model:\n";
    os << "  default: " << yamlScalar(def.id) << "\n";
    os << "  upstream_id: " << yamlScalar(def.upstreamId) << "\n";
    os << "  provider: " << yamlScalar(pname) << "\n";
    // 纯自定义端点 (无内核 base_url_env_var): base_url 走 config.yaml model 段
    // (内核 bare-custom 对该值有信任校验, 原生厂商走 .env 的 <X>_BASE_URL, 不写这里)。
    if (defProv && defProv->baseUrlEnvVar.empty() && !defProv->baseUrl.empty())
        os << "  base_url: " << yamlScalar(defProv->baseUrl) << "\n";

    if (opt.includeParams && def.params.temperature.has_value()) {
        os << "  temperature: " << *def.params.temperature << "\n";
    }

    os << "\nagent:\n";
    os << "  max_turns: 90\n";
    os << "\nterminal:\n";
    os << "  backend: local\n";
    os << "  timeout: 180\n";
    os << "\ncompression:\n";
    os << "  enabled: true\n";
    os << "  threshold: 0.5\n";
    os << "  target_ratio: 0.2\n";
    os << "\ndisplay:\n";
    os << "  skin: default\n";
    os << "  tool_progress: true\n";
    os << "  show_cost: true\n";
    os << "\nmemory:\n";
    os << "  memory_enabled: true\n";
    os << "  user_profile_enabled: true\n";

    // ---- 模板段 (mcp_servers 等, 原样附加) ----
    std::string tpl = readFile(templateFile_);
    if (!tpl.empty()) {
        os << "\n" << tpl;
        if (tpl.back() != '\n') os << "\n";
    }

    if (!writeTextAtomic(yamlOut_, os.str())) {
        return failVoid(41, "写入 config.yaml 失败", yamlOut_.u8string());
    }
    return okVoid();
}

VoidResult ConfigRenderer::renderAll(const ModelConfig& cfg, const RuntimeConfig& rt,
                                     const RenderOptions& opt) const {
    // 先备份, 任一失败可还原
    bool bak1 = copyToBak(envOut_);
    bool bak2 = copyToBak(yamlOut_);

    auto r1 = renderEnv(cfg);
    if (!r1.ok()) {
        if (bak1 || bak2) restore();
        return r1;
    }
    auto r2 = renderConfigYaml(cfg, rt, opt);
    if (!r2.ok()) {
        if (bak1 || bak2) restore();
        return r2;
    }
    return okVoid();
}

VoidResult ConfigRenderer::restore() const {
    std::error_code ec;
    bool any = false;
    for (const auto& p : {envOut_, yamlOut_}) {
        fs::path bak = p;
        bak += ".bak";
        if (fs::exists(bak, ec)) {
            fs::copy_file(bak, p, fs::copy_options::overwrite_existing, ec);
            any = true;
        }
    }
    return any ? okVoid() : failVoid(42, "没有可用的 .bak 备份");
}

bool ConfigRenderer::externallyModified() const {
    // 与 .bak 做内容比对: 不一致说明渲染后被外部改过
    std::error_code ec;
    for (const auto& p : {envOut_, yamlOut_}) {
        fs::path bak = p;
        bak += ".bak";
        if (fs::exists(bak, ec) && fs::exists(p, ec)) {
            if (readFile(p) != readFile(bak)) return true;
        }
    }
    return false;
}

} // namespace hs
