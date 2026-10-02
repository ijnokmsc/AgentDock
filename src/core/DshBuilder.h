#pragma once
// ADR-008: DeepSeek Harness (dsh) 便携包从 0 构建。
//
// 与 scripts/build_dsh_portable.py 步骤一一对应 (Python 版是参照实现,
// I:\PortableAgent\DSH 现役包即其产物):
//   1. 下载 nodejs.org node zip (+SHASUMS256.txt 校验)  -> studio/cache/downloads
//   2. 解包 -> <target>/node (顶层目录自动归位, 同 Updater::applyNodeUpdate)
//   3. npm install @deepseek-ai/dsh@<锁版本> --prefix <target>/dsh
//      (用包内 node 自带的 npm-cli.js; 环境钉死, 用户 ~/.npmrc 隔离)
//   4. 写 Dsh.bat (ASCII+CRLF, 环境表与 DshRuntime::pinnedEnv 同源)
//   5. 建 data/dsh + _home + studio/cache 目录
//   6. dsh --dump-default-config 预初始化 profiles/web (首启零网络)
//   7. 写 runtime-manifest.json
//   8. 校验: dsh --version + Dsh.bat --version
//
// dsh 处于 0.1.x RC (官方明示将有破坏性变更) -> 版本必须显式指定,
// resolveLatest() 只用于展示, 不自动决定装什么。

#include <string>
#include <vector>
#include <functional>
#include <filesystem>

namespace fs = std::filesystem;

namespace hs::dsh {

struct BuildOptions {
    fs::path    targetDir;                  // 空 = portableRoot()/DSH
    std::string dshVersion  = "0.1.5-rc.2"; // 精确版本锁 (RC 阶段不漂移)
    std::string nodeVersion = "v24.21.0";   // LTS; 官方要求 >=22.5 (node:sqlite)
    bool        useCache    = true;         // 复用 studio/cache/downloads 同名文件
};

struct BuildProgress {
    int         percent = 0;
    std::string stage;
};

struct BuildResult {
    bool        ok = false;
    bool        cancelled = false;
    std::string message;
};

struct RemoteInfo {
    bool                       ok = false;
    std::string                message;
    std::string                dshLatest;              // npm dist-tags.latest
    std::vector<std::string>   dshVersions;            // 全部可装版本 (新->旧)
    std::string                nodeLts;                // nodejs.org 最新 LTS ("v24.21.0")
};

// 只读网络, 后台线程调用
RemoteInfo resolveLatest();

// 网络 + 大文件 + npm 子进程, 必须后台线程调用。
// 取消: 阶段边界生效 (npm install 阶段不可中断, 与 Python 参照实现一致)。
BuildResult run(const BuildOptions& opts,
                std::function<void(const BuildProgress&)> progress = {},
                std::function<bool()> cancelled = nullptr);

// ---- 组件更新 (更新中心 ADR-008; 前置条件: 调用方先停 dsh 进程) ----
// dsh 核心 = 清空 dsh/node_modules 重装指定版本; 失败自动回滚备份。
// version 为精确 npm 版本号 (如 0.1.5-rc.2)。
BuildResult updateCore(const std::string& version,
                       std::function<void(const BuildProgress&)> progress = {});
// DSH 包内 Node 运行时 = 下载替换 node/ (同构建流程 + 备份回滚)。
// version 形如 "24.21.0" 或 "v24.21.0"。
BuildResult updateNode(const std::string& version,
                       std::function<void(const BuildProgress&)> progress = {});

} // namespace hs::dsh
