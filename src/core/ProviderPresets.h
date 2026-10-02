// ProviderPresets.h —— 内置厂商预设表
//
// 字段约定来源 (2026-09-29 对 hermes-agent 内核源码实测, 见 python/hermes_cli/providers.py
// 的 HERMES_OVERLAYS / _host_derived_api_key 与 runtime_provider_backends.py):
//   - 原生厂商: config.yaml model.provider = providerName (内核 provider id);
//     Key 走 envVar; Base URL 覆盖走 baseUrlEnvVar (内核 base_url_env_var 约定)。
//   - 自定义厂商 (内核无原生条目): providerName 为空 → 渲染 provider: custom,
//     base_url 进 config.yaml model 段; Key 变量名必须匹配内核 host 推导
//     (api.deepseek.com → DEEPSEEK_API_KEY, api.siliconflow.cn → SILICONFLOW_API_KEY)。
#pragma once
#include <string>
#include <vector>

namespace hs {

struct PresetModel {
    const char* upstreamId;     // 发给上游的 model 名
    const char* displayName;    // 展示名
};

struct ProviderPreset {
    const char* id;             // launcher provider id (新增时预填)
    const char* displayName;
    const char* group;          // 国内主流 / 国际主流 / 本地自托管 / 通用
    const char* providerName;   // 内核 provider id; "" = custom (OpenAI 兼容自定义)
    const char* kind;           // launcher kind 标签
    const char* baseUrl;        // 官方端点; "" = 用内核默认
    const char* baseUrlEnvVar;  // 内核 base URL 覆盖变量; "" = 无 (custom 走 config.yaml base_url)
    const char* envVar;         // API Key env; "" = keyless
    bool        keyRequired;
    const char* keyUrl;         // 申请 Key 页面; "" = 无
    const char* notes;
    std::vector<PresetModel> models;   // 常用模型 (一键批量添加)
};

const std::vector<ProviderPreset>& providerPresets();

// config.yaml model.provider 的取值: 显式 providerName > 已知内核 id > custom。
// 兜底 custom 是内核语义: 未知名字会直接 AuthError(invalid_provider)。
std::string resolveKernelProviderName(const std::string& id, const std::string& providerName);

}  // namespace hs
