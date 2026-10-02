#include "JsonStore.h"

#include <windows.h>

#include <fstream>
#include <sstream>

namespace hs {

bool JsonStore::ensureParent(const fs::path& path) {
    std::error_code ec;
    fs::path parent = path.parent_path();
    if (parent.empty()) return true;
    fs::create_directories(parent, ec);
    return !ec;
}

std::optional<nlohmann::json> JsonStore::load(const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return std::nullopt;

    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    try {
        return nlohmann::json::parse(ss.str());
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

hs::VoidResult JsonStore::save(const fs::path& path, const nlohmann::json& j) {
    if (!ensureParent(path)) {
        return failVoid(1, "无法创建目录", path.parent_path().u8string());
    }

    std::string text;
    try {
        text = j.dump(2);
    } catch (const std::exception& e) {
        return failVoid(2, "JSON 序列化失败", e.what());
    }

    fs::path tmp = path;
    tmp += ".tmp";

    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return failVoid(3, "无法写入临时文件", tmp.u8string());
        out << text;
        out.flush();
        if (!out) return failVoid(3, "写入临时文件失败", tmp.u8string());
    }

    // 原子替换。MOVEFILE_REPLACE_EXISTING + WRITE_THROUGH 保证替换与落盘
    if (!MoveFileExW(tmp.wstring().c_str(),
                     path.wstring().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ec2;
        fs::remove(tmp, ec2);
        return failVoid(4, "原子替换失败", std::to_string(GetLastError()));
    }
    return okVoid();
}

bool JsonStore::appendLine(const fs::path& path, const nlohmann::json& j) {
    if (!ensureParent(path)) return false;
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) return false;
    try {
        out << j.dump() << "\n";
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

} // namespace hs
