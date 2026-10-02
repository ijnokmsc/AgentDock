#pragma once

#include <string>
#include <filesystem>
#include <optional>

#include <nlohmann/json.hpp>

#include "../core/Types.h"   // VoidResult / okVoid / failVoid

namespace fs = std::filesystem;

namespace hs {

// JSON 原子读写。
//
// 写协议: 写 .tmp -> 刷新到磁盘 -> MoveFileEx 原子替换 -> 成功后删旧 .bak
// 目的: 便携包可能在 U 盘上被直接拔出, 断电/强杀时主文件必须保持完整。
class JsonStore {
public:
    // 读取; 文件不存在返回 nullopt (不是错误, 交给调用方决定默认值)
    static std::optional<nlohmann::json> load(const fs::path& path);

    // 原子写入; 失败返回错误信息
    static hs::VoidResult save(const fs::path& path, const nlohmann::json& j);

    // 追加一行 JSON (用于 history.jsonl)
    static bool appendLine(const fs::path& path, const nlohmann::json& j);

    // 确保父目录存在
    static bool ensureParent(const fs::path& path);
};

} // namespace hs
