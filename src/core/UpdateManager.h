#pragma once
// 更新应用链路: 下载 → SHA-256 校验 → 备份 → 原子替换 → 回滚。
//
// 通用机制:
//   backupComponent()  把组件当前状态备份到 <studio>/backups/<component>/<ver>/
//   rollback()         从备份恢复组件
//
// 组件特定的"如何应用更新"由调用方 (Updater) 根据组件类型实现, 但备份/回滚
// 统一由本模块保证。内核更新需重装 pip editable (高风险), 运行时替换 exe (低风险)。

#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <filesystem>

namespace fs = std::filesystem;

namespace hs {

class UpdateManager {
public:
    UpdateManager() = default;

    // 备份组件当前目录到 backups/<name>/<tag>/
    // src 为组件根目录, name 为备份命名空间, tag 为版本标识。
    // onProgress 可选: 每复制若干文件回调一次 (参数 = 已复制字节数),
    // 大目录 (如 dsh npm 前缀数百 MB) 备份耗时 1-2 分钟, UI 需要此回调才有反馈。
    // 返回备份目录路径 (空 = 失败)。
    fs::path backupDir(const std::string& name, const std::string& tag) const;
    fs::path backupComponent(const fs::path& src, const std::string& name,
                             const std::string& tag, std::string* err = nullptr,
                             const std::function<void(std::uintmax_t)>& onProgress = {});

    // 从备份恢复组件。backupPath 由 backupComponent 返回。
    // 目标 = 备份时记录的原始位置 (存在 backup/.manifest.json)。
    bool rollback(const fs::path& backupPath, std::string* err = nullptr);

    // 删除备份目录。带防护: 仅允许删除 backups 根下的目录, 拒绝其他路径。
    bool removeBackup(const fs::path& backupPath, std::string* err = nullptr);

    // 原子替换: 把 src 的内容替换到 dest 目录 (先备份旧版到 backups/<name>/<ver>/)。
    // 失败自动回滚。progress 用于 UI 反馈。
    struct ReplaceResult {
        bool        ok = false;
        std::string message;
        fs::path    backup;   // 本次备份位置 (可用来回滚)
    };
    ReplaceResult atomicReplace(const fs::path& src, const fs::path& dest,
                                const std::string& name, const std::string& ver,
                                std::function<void(int percent, const std::string& stage)> progress = {});

    // 列出某个组件的可用备份
    std::vector<fs::path> listBackups(const std::string& name) const;

private:
    fs::path backupsRoot() const;
};

} // namespace hs
