#pragma once
// ADR-008: DeepSeek Harness (dsh) 运行时支持。
//
// dsh 是纯 Node 应用 (npm @deepseek-ai/dsh), 便携化三要素 (均实测):
//   1. DSH_HOME 环境变量 = 官方数据根 hook, 无需 HOME junction;
//   2. 其 Windows 模块回退链接是 junction 且每次启动自动重指向 -> 包可移动;
//   3. 会话 token 只打印在 stdout 首行 -> start() 用匿名管道捕获。
//
// 与 Hermes 路径零耦合: 不依赖 hs::Paths (那是 Hermes 布局解析),
// 根目录 = portableRoot()/DSH, 由壳层传入 Init。

#include <string>
#include <vector>
#include <functional>
#include <filesystem>

namespace fs = std::filesystem;

namespace hs::dsh {

// ---- 路径 (root 由壳层 Init 注入, 通常 = portableRoot()/DSH) ----
void        setRoot(const fs::path& root);
fs::path    root();                 // <portableRoot>/DSH
fs::path    nodeExe();              // <root>/node/node.exe
fs::path    binJs();                // <root>/dsh/node_modules/@deepseek-ai/dsh/lib/bin.js
fs::path    homeDir();              // <root>/data/dsh        (DSH_HOME)
fs::path    batPath();              // <root>/Dsh.bat

bool installed();                   // node.exe + bin.js 都在

// 钉死环境表 (Dsh.bat 模板与运行时注入共用同一份事实, 不漂移):
// DSH_HOME / HOME / USERPROFILE / APPDATA / LOCALAPPDATA / TEMP / TMP /
// NPM_CONFIG_CACHE / PNPM_HOME
std::vector<std::pair<std::string, std::string>> pinnedEnv();
std::vector<std::pair<std::string, std::string>> pinnedEnvAt(const fs::path& root);
// 建出环境表指向的全部目录 (TEMP 不存在会让 runtime 直接失败)
void ensureDirs();
void ensureDirsAt(const fs::path& root);

// ---- 版本探测 (runAndCapture, 秒级) ----
struct Versions {
    std::string node;      // "v24.21.0"
    std::string dsh;       // "0.1.5-rc.2"
    std::string error;     // 空 = OK
};
Versions queryVersions();

// ---- 启动/停止 ----
// 启动 node bin.js web --no-open --port <port> (隐藏窗口, 钉死环境)。
// 后台线程持续读子进程 stdout, 捕获 "dsh web: http://...?token=..." 存入 tokenUrl()。
struct StartResult {
    bool        ok = false;
    std::string message;
    unsigned long pid = 0;
};
StartResult start(int port, std::string* err = nullptr);
void        stop(int port);         // 记录 pid 优先, 其次端口反查, killTree 收尾
bool        isUp(int port);         // 端口监听即视为 up (与启动方式无关)
std::wstring tokenUrl();            // 捕获到的带 token 完整 URL; 未捕获 = 空
void        resetSession();         // 停止后清 token

// ---- 常量 ----
constexpr int kDefaultPort = 3080;

// ---- 插件管理 (profiles/web, dsh plugin 转发内置 pnpm; ADR-008) ----
// dsh 自带打包版 pnpm, 无需预装; pnpm store 由 pinnedEnv 钉在包内。
// 列表来源 = profiles/web/package.json 的 dependencies (dsh plugin add 维护)。
struct PluginInfo {
    std::string name;       // npm 包名
    std::string range;      // package.json 里声明的版本范围 (如 ^2.1.3)
};
std::vector<PluginInfo> pluginList();              // 空 = 无第三方插件
struct PluginOpResult {
    bool        ok = false;
    std::string output;                            // dsh/pnpm 原始输出 (截尾)
};
PluginOpResult pluginAdd(const std::string& pkg);    // dsh plugin --profile web add <pkg>
PluginOpResult pluginRemove(const std::string& pkg); // dsh plugin --profile web remove <pkg>
fs::path pluginProfileDir();                       // <root>/data/dsh/profiles/web

// ---- 插件依赖自检/自装 (Git 仓库形插件需要 pnpm; ADR-008) ----
std::string pnpmVersion();                         // 空 = 不可用 (包内 node/ 与系统 PATH 均无)
bool        gitAvailable();                        // 系统 PATH 或便携包旁 Hermes PortableGit
PluginOpResult installPnpm();                      // npm -g pnpm --prefix <root>/node (装入包内)

// ---- HTTP 就绪探测 ----
// 端口监听 ≠ 服务可用: dsh 监听后 ~5s 才真正应答 HTTP。任何 HTTP 响应
// (含 401 fence) 都算就绪; 连接失败/超时 = 未就绪。
bool httpReady(int port);

} // namespace hs::dsh
