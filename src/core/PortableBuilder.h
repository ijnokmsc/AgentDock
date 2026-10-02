#pragma once
// ADR-004: 启动器一键构建便携版。
//
// 目标 —— 不依赖任何第三方打包好的便携版, 只用**官方 releases** 的两样东西,
// 加上启动器已有的底层能力 (HttpClient / Archive / Hash), 组装出一个完整可跑
// 的便携包:
//
//   1. runtime release   tag 形如 `hermes-0.21.3-runtime`
//        · hermes-runtime-win-x64.json     清单 (版本 + sha256 + 大小)
//        · hermes-runtime-hermes-agent-0.21.3-win-x64.tar.gz
//          解开就是 ./python (解释器 base/ + venv/ + hermes-agent 源码)
//                    ./node   ./git
//
//   2. web-ui release    tag 形如 `v0.7.23`
//        · hermes-web-ui-0.7.23.json
//        · hermes-web-ui-0.7.23.tar.gz      解开是 ./webui (自带 node_modules)
//
// 两者都直接解到目标根目录, 就得到 Paths.h 里描述的 OFFICIAL 布局。
// 剩下的「让它可以被搬走」的部分 (沙箱、junction、Hermes.bat、pyvenv.cfg)
// 全部交给 PortableLayout —— 那是构建期与运行期共用的同一份实现。
//
// 版本发现: 从 GitHub API 列 releases, 取**最新**一个带
// `hermes-runtime-<platform>.json` 的 runtime release, 以及最新一个带
// `hermes-web-ui-<ver>.json` 的 web-ui release —— 不硬编码版本号。

#include <string>
#include <functional>
#include <filesystem>

namespace fs = std::filesystem;

namespace hs {

struct BuildOptions {
    fs::path    targetDir;                  // 目标目录; 空 = 启动器所在目录 (portableRoot)
    std::string platform      = "win-x64";  // 目前只支持 win-x64
    bool        useCache      = true;       // 复用 studio/cache/downloads 里的同名文件
    bool        verifySha256  = true;
    std::string githubMirror;               // 可选: 下载加速前缀, 如 https://ghfast.top/
};

struct BuildProgress {
    int         percent = 0;
    std::string stage;
};

struct BuildResult {
    bool        ok = false;
    bool        cancelled = false;
    std::string message;
    fs::path    targetDir;
    std::string runtimeVersion;
    std::string webUiVersion;
};

class PortableBuilder {
public:
    using ProgressFn = std::function<void(const BuildProgress&)>;
    using CancelFn   = std::function<bool()>;   // 返回 true = 请求取消

    // 已经解析好的远端版本信息 (构建页可以先调 resolveLatest() 展示给用户)
    struct RemoteInfo {
        bool        ok = false;
        std::string message;
        std::string runtimeTag;
        std::string runtimeVersion;
        std::string runtimeUrl;
        std::string runtimeSha256;
        long long   runtimeSize = 0;
        std::string webUiTag;
        std::string webUiVersion;
        std::string webUiUrl;
        std::string webUiSha256;
        long long   webUiSize = 0;
        std::string manifestJson;   // runtime 清单原文 (构建后写入包内留档)
    };

    // 查最新版本 (只读网络, 不写盘)。可在后台线程调用。
    RemoteInfo resolveLatest(const std::string& platform = "win-x64",
                             const std::string& mirror = {}) const;

    // 执行构建。含网络 + 大文件 + 数万次写盘, 必须在后台线程调用。
    BuildResult run(const BuildOptions& opts,
                    ProgressFn progress = {},
                    CancelFn cancelled = {}) const;

    // 建 pyvenv.cfg 的绝对 home (python-build-standalone 的 trampoline 会把
    // 相对 home 按 CWD 解析, 所以每次构建/启动都必须绝对化)。
    static bool fixPyvenvHome(const fs::path& root, std::string* err = nullptr);
};

} // namespace hs
