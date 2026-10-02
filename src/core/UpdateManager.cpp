#include "UpdateManager.h"
#include "../app/Paths.h"
#include "../storage/JsonStore.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <algorithm>

namespace {

// 递归复制目录树 (覆盖); onFile 可选: 每复制一个文件回调 (累计字节)
bool copyTree(const fs::path& from, const fs::path& to, std::string* err = nullptr,
              const std::function<void(std::uintmax_t)>& onFile = {}) {
    std::error_code ec;
    if (!fs::is_directory(from, ec)) {
        if (err) *err = "源目录不存在: " + from.string();
        return false;
    }
    fs::create_directories(to, ec);
    std::uintmax_t copied = 0;
    for (fs::recursive_directory_iterator it(from, fs::directory_options::skip_permission_denied, ec), end;
         it != end; ++it) {
        const auto& src = it->path();
        auto rel = fs::relative(src, from, ec);
        auto dst = to / rel;
        if (it->is_directory(ec)) {
            fs::create_directories(dst, ec);
        } else if (it->is_regular_file(ec)) {
            fs::create_directories(dst.parent_path(), ec);
            if (!fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec)) {
                if (err) *err = "复制失败: " + src.string() + " - " + ec.message();
                return false;
            }
            copied += it->file_size(ec);
            if (onFile) onFile(copied);
        }
        if (ec) { if (err) *err = "复制出错: " + ec.message(); return false; }
    }
    return true;
}

bool removeTree(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
    return !ec;
}

} // namespace

namespace hs {

fs::path UpdateManager::backupsRoot() const {
    return Paths::backupDir();   // <root>/studio/backups
}

fs::path UpdateManager::backupDir(const std::string& name, const std::string& tag) const {
    return backupsRoot() / name / tag;
}

fs::path UpdateManager::backupComponent(const fs::path& src, const std::string& name,
                                        const std::string& tag, std::string* err,
                                        const std::function<void(std::uintmax_t)>& onProgress) {
    if (src.empty() || !fs::is_directory(src)) {
        if (err) *err = "组件目录不存在: " + src.string();
        return {};
    }
    fs::path dest = backupDir(name, tag);
    std::error_code ec;
    fs::remove_all(dest, ec);   // 清掉同名旧备份
    if (!copyTree(src, dest, err, onProgress)) return {};

    // 记录原始位置 (供回滚)
    json manifest;
    manifest["name"] = name;
    manifest["tag"] = tag;
    manifest["originalPath"] = fs::absolute(src).string();
    manifest["backedUpAt"] = "";
    JsonStore::save(dest / ".manifest.json", manifest);
    return dest;
}

bool UpdateManager::rollback(const fs::path& backupPath, std::string* err) {
    std::error_code ec;
    auto manifest = JsonStore::load(backupPath / ".manifest.json");
    if (!manifest) {
        if (err) *err = "备份无清单: " + backupPath.string();
        return false;
    }
    std::string orig = manifest->value("originalPath", std::string{});
    if (orig.empty()) { if (err) *err = "备份清单缺少原始路径"; return false; }

    // 先备份当前 (损坏的) 版本, 再恢复旧版
    fs::path brokenBak = backupPath.parent_path() / ("_broken_" + backupPath.filename().string());
    fs::remove_all(brokenBak, ec);
    if (fs::is_directory(orig)) fs::rename(orig, brokenBak, ec);

    fs::create_directories(orig, ec);
    if (!copyTree(backupPath, orig, err)) {
        // 恢复失败, 尝试还原 broken
        if (fs::is_directory(brokenBak) && !fs::is_directory(orig)) fs::rename(brokenBak, orig, ec);
        return false;
    }
    // 清理临时
    fs::remove_all(brokenBak, ec);
    return true;
}

bool UpdateManager::removeBackup(const fs::path& backupPath, std::string* err) {
    std::error_code ec;
    if (backupPath.empty() || !fs::is_directory(backupPath, ec)) {
        if (err) *err = "备份不存在";
        return false;
    }
    // 防护: 只允许删 backups 根下的路径 (reject ../ 与任意目录)
    auto root = fs::weakly_canonical(backupsRoot(), ec);
    auto target = fs::weakly_canonical(backupPath, ec);
    if (ec) { if (err) *err = ec.message(); return false; }
    auto rootIt = root.begin(), rootEnd = root.end();
    auto tgtIt = target.begin(), tgtEnd = target.end();
    for (; rootIt != rootEnd; ++rootIt, ++tgtIt) {
        if (tgtIt == tgtEnd || *rootIt != *tgtIt) {
            if (err) *err = "路径不在备份根下, 已拒绝删除";
            return false;
        }
    }
    if (tgtIt == tgtEnd) { if (err) *err = "不能删除备份根目录本身"; return false; }
    fs::remove_all(target, ec);
    if (ec) { if (err) *err = "删除失败: " + ec.message(); return false; }
    return true;
}

std::vector<fs::path> UpdateManager::listBackups(const std::string& name) const {
    std::vector<fs::path> out;
    std::error_code ec;
    fs::path root = backupsRoot() / name;
    if (!fs::is_directory(root, ec)) return out;
    for (fs::directory_iterator it(root, ec), end; it != end; ++it) {
        if (it->is_directory(ec)) out.push_back(it->path());
    }
    std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b){
        return a.filename().string() < b.filename().string();
    });
    return out;
}

UpdateManager::ReplaceResult UpdateManager::atomicReplace(
        const fs::path& src, const fs::path& dest,
        const std::string& name, const std::string& ver,
        std::function<void(int, const std::string&)> progress) {
    ReplaceResult r;

    if (progress) progress(10, "备份旧版本");
    r.backup = backupComponent(dest, name, ver);
    if (r.backup.empty()) {
        r.message = "备份旧版本失败";
        return r;
    }

    if (progress) progress(40, "替换组件");
    // 先删目标, 再从 src 复制 (src 是已解压的临时新版本)
    std::error_code ec;
    fs::remove_all(dest, ec);
    fs::create_directories(dest, ec);
    if (!copyTree(src, dest)) {
        // 失败: 回滚到备份
        r.message = "替换失败, 尝试回滚";
        if (rollback(r.backup)) {
            r.message = "替换失败, 已回滚到原版本";
        }
        return r;
    }

    if (progress) progress(90, "完成");
    r.ok = true;
    r.message = "已更新到 " + ver;
    return r;
}

} // namespace hs
