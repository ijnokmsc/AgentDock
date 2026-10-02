#pragma once
// 解压能力: 支持 zip 与 tar.gz。
//
// - zip:  用 miniz (thirdparty/miniz, public domain) 解压
// - tar.gz: 用 miniz 的 tdefl_inflate 解 gzip 流, 再内置解析 tar 格式 (UStar)
//
// 为 M3 更新中心服务: 下载的 GitHub Release 源码包 (zipball=zip / tarball=tar.gz)
// 解压后替换目标组件目录。
//
// 安全: 解压时拒绝路径穿越 (.. 与绝对路径), 防恶意压缩包逃逸。

#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <functional>

namespace fs = std::filesystem;

namespace hs::arc {

// 解包运行控制 (可选):
//   cancel   — 轮询调用, 返回 true 时立即终止外部解压进程 (tar.exe)
//   生命周期保证: 外部解压进程挂在 Job Object 上 (KILL_ON_JOB_CLOSE),
//   启动器进程无论正常退出还是被强杀, 解压进程都会随之终止。
struct ExtractControl {
    const std::function<bool()>* cancel = nullptr;
};

// 解压 archive 到目标目录 (覆盖同名文件)。
// 返回错误信息 (空 = 成功)。
std::optional<std::string> extract(const fs::path& archive, const fs::path& destDir,
                                   const ExtractControl* ctl = nullptr);

// 根据扩展名判断并解压 (自动识别 .zip / .tar.gz / .tgz / .tar)
std::optional<std::string> extractAuto(const fs::path& archive, const fs::path& destDir,
                                       const ExtractControl* ctl = nullptr);

} // namespace hs::arc
