#pragma once

#include "Types.h"
#include "../app/Paths.h"
#include <vector>
#include <optional>
#include <functional>

namespace hs {

// 语义化版本比较。同时支持:
//   "0.20.5"      SemVer
//   "20250909"    python-build-standalone 的日期版本
//   "v24.11.1"    带 v 前缀
int compareVersion(const std::string& a, const std::string& b);

// 更新中心。
//
// M0 阶段只做本地版本探测与版本比对, 远端检查/下载/应用留到 M3。
// 这里先把组件注册表与数据结构定死, UI 可以先把「当前版本」显示出来。
class Updater {
public:
    struct ComponentDef {
        ComponentId id;
        std::string name;
        std::string localVersionProbe;   // 探测方式说明, 展示给用户
        bool        highRisk = false;    // Python 运行时更新会让 venv 失效, 属高风险
    };

    Updater();

    // 探测全部组件的本地版本 (会执行 --version, 较慢, 应在后台线程调用)
    std::vector<ComponentState> scanLocal();

    // 探测本地版本 + 远端最新版本, 填充 latestVersion 与 updateAvailable。
    // 含网络请求, 必须在后台线程调用。
    std::vector<ComponentState> scanWithRemote(const std::string& channel = "stable");

    // 已知组件的元信息
    static const std::vector<ComponentDef>& definitions();
    static std::optional<ComponentDef> defOf(ComponentId id);

    // 远端版本检查 (M3 实现)
    struct RemoteResult {
        bool        ok = false;
        std::string latestVersion;
        std::string message;
    };
    RemoteResult checkRemote(ComponentId id, const std::string& channel);

    // ---- 更新应用 (M3) ----
    // 结果: ok + message + 可选 backup(用于回滚)
    struct ApplyResult {
        bool        ok = false;
        std::string message;
        fs::path    backup;
    };

    // 内核更新: 下载 GitHub zipball → 解压 → 备份旧版 → 覆盖源码树。
    // (保持目录名不变, editable finder 自动解析新源码, 无需 pip 重装)。
    //
    // **官方布局要点**: <root>/python 既是 hermes-agent 源码树根, 又装着
    // 解释器 base/ 与 venv/ —— 所以不能整目录替换, 必须跳过 base/ 与 venv/,
    // 只做逐项覆盖。旧布局 (<root>/hermes-agent) 仍是整目录替换。
    // 含网络 + 大文件, 必须在后台线程调用。
    ApplyResult applyKernelUpdate(const std::string& targetVersion,
                                  std::function<void(int, const std::string&)> progress = {});

    // Web UI 更新: 下载 npm 包 → 备份 → 替换 webui/ (或旧布局的
    // node_modules/hermes-web-ui)
    ApplyResult applyWebUiUpdate(const std::string& targetVersion,
                                 std::function<void(int, const std::string&)> progress = {});

    // Node 运行时更新: 下载 nodejs.org 官方发行版 → 备份 → 替换 node/ 目录。
    // 官方布局下 node/ 是纯净发行版 (node_modules 只有 npm/corepack), 可整目录替换。
    // 含网络 + 大文件, 必须在后台线程调用。
    ApplyResult applyNodeUpdate(const std::string& version,
                                std::function<void(int, const std::string&)> progress = {});

    // 启动器自更新: 从发布仓库下载新版 exe, 改名腾位自替换 (.old.exe 保底),
    // 重启后生效。含网络 + 文件替换, 必须在后台线程调用。
    ApplyResult applyLauncherUpdate(const std::string& targetVersion,
                                    std::function<void(int, const std::string&)> progress = {});

    // 列出 nodejs.org 可用的 Node 版本 (从新到旧, 最多 maxCount 个)。
    // 用于「更新选中时让用户挑版本」。含网络请求, 在后台线程调用。
    std::vector<std::string> listNodeVersions(size_t maxCount = 30);

private:
    std::optional<std::string> probeKernel() const;
    std::optional<std::string> probeWebUi() const;
    std::optional<std::string> probePython() const;
    std::optional<std::string> probeNode() const;
};

} // namespace hs
