#pragma once

#include "Types.h"
#include <filesystem>

namespace fs = std::filesystem;

namespace hs {

// 把自有 JSON 配置渲染成 Hermes 内核认的两个文件:
//   data/.env         <- 所有启用 Provider 的 API Key
//   data/config.yaml  <- 动态段 + 模板段(mcp_servers 等) 原样附加
//
// 写前统一先备份为 .bak, 渲染失败可一键还原。
class ConfigRenderer {
public:
    ConfigRenderer(fs::path envOut, fs::path yamlOut, fs::path templateFile);

    // 渲染 .env
    VoidResult renderEnv(const ModelConfig& cfg) const;

    // 渲染 config.yaml
    struct RenderOptions {
        bool includeParams = false;   // 内核对未知字段可能严格, 默认不写 params
    };
    VoidResult renderConfigYaml(const ModelConfig& cfg,
                                const RuntimeConfig& rt,
                                const RenderOptions& opt = {}) const;

    // 全量渲染 (先备份, 再写两个文件)
    VoidResult renderAll(const ModelConfig& cfg, const RuntimeConfig& rt,
                         const RenderOptions& opt = {}) const;

    // 从 .bak 还原
    VoidResult restore() const;

    // 检测外部改动: 目标文件 mtime 是否新于上次渲染记录
    bool externallyModified() const;

private:
    fs::path envOut_;
    fs::path yamlOut_;
    fs::path templateFile_;
};

} // namespace hs
