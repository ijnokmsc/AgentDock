#pragma once

#include <string>
#include <optional>
#include <filesystem>

namespace fs = std::filesystem;

namespace hs::crypto {

// 文件 SHA-256 (BCrypt/CNG, 流式读取, 不整文件载入内存)
// 返回 64 位小写十六进制串
std::optional<std::string> sha256File(const fs::path& path);

} // namespace hs::crypto
