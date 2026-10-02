#pragma once

#include "Types.h"
#include <filesystem>

namespace fs = std::filesystem;

namespace hs {

// 模型配置的真相源 (data/studio/models.json)。
//
// 职责:
//   - 加载/保存 (原子写)
//   - Provider / Model 的增删改
//   - 默认模型的唯一性维护
//   - 首次运行从旧配置中心产物 (.env / config.yaml) 迁移
class ModelRegistry {
public:
    explicit ModelRegistry(fs::path file);

    VoidResult load();
    VoidResult save() const;

    const ModelConfig& config() const { return cfg_; }
    ModelConfig&       mutableConfig() { return cfg_; }

    // ---- Provider ----
    std::vector<Provider> providers() const { return cfg_.providers; }
    std::optional<Provider> provider(const std::string& id) const;
    VoidResult addProvider(const Provider& p);
    VoidResult updateProvider(const Provider& p);
    VoidResult removeProvider(const std::string& id);   // 连带删除其下模型

    // ---- Model ----
    std::vector<Model> models() const { return cfg_.models; }
    std::vector<Model> modelsOf(const std::string& providerId) const;
    std::optional<Model> model(const std::string& id) const;
    std::optional<Model> defaultModel() const;
    VoidResult addModel(const Model& m);
    VoidResult updateModel(const Model& m);
    VoidResult removeModel(const std::string& id);
    VoidResult setDefault(const std::string& modelId);

    // ---- 迁移 ----
    // 从现有 data/.env + data/config.yaml 生成初始 models.json。
    // 只在 models.json 不存在时调用。
    struct MigrationResult {
        int providersImported = 0;
        int modelsImported    = 0;
        bool configTemplateCreated = false;
        std::vector<std::string> warnings;
    };
    MigrationResult migrateFromLegacy(const fs::path& envFile,
                                      const fs::path& configFile,
                                      const fs::path& templateOut);

    // 校验: 是否有可用的 provider + 默认模型
    std::vector<CheckItem> validate() const;

private:
    fs::path    file_;
    ModelConfig cfg_;
};

} // namespace hs
