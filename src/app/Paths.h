#pragma once

#include <string>
#include <utility>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

namespace hs {

// Hermes 便携包的路径解析。
//
// ============================ 两种布局 ============================
//
//  OFFICIAL —— 官方预编译 runtime 布局, 也是 PortableBuilder 的构建产物
//              (ADR-004, 启动器的唯一目标布局):
//
//      <root>/AgentDock.exe               启动器 (锚点, portableRoot)
//      <root>/python/base/python.exe      python-build-standalone 解释器
//      <root>/python/venv/Scripts/        venv (pyvenv.cfg home 指向 base)
//      <root>/python/hermes_cli/          hermes-agent **源码树根**就在 python/
//      <root>/python/run_agent.py
//      <root>/webui/dist/server/index.js  web-ui (自带 node_modules)
//      <root>/node/node.exe               node 纯净发行版 (node_modules 只有 npm)
//      <root>/git/ PortableGit
//      <root>/data/                       HERMES_HOME
//      <root>/_home/                      HOME 劫持沙箱 (_home\.hermes -> data)
//      <root>/studio/ <root>/plugin/      启动器私有 (构建期/首次运行创建)
//
//  注意: 官方布局里 hermes-agent 源码与运行时 (base/venv) **混在同一个
//  python/ 目录下**, 所以做「内核更新」时不能整目录替换, 必须跳过 base/ venv/。
//
//  LEGACY —— 旧第三方搬运包, 仅保留兼容读取能力:
//      <root>/hermes-agent/  <root>/venv/  <root>/node/node_modules/hermes-web-ui/
//
// 判定优先级: setRoot() 显式指定 > exe 目录 > exe 子目录 Hermes/ > 向上逐级。
class Paths {
public:
    // 显式指定根目录; 返回是否通过指纹校验
    static bool setRoot(const fs::path& root);

    // 自动探测 (按优先级 2 -> 3)
    static bool autoDetect();

    // 便携包根 = AgentDock.exe 所在目录 (锚点, 可重定位)
    static fs::path portableRoot();

    // Hermes 内容根 = portableRoot / hermesRoot 相对路径
    static const fs::path& root();
    static bool            isValid();

    // 布局判定 (根已解析)
    static bool isOfficialLayout();   // OFFICIAL (官方 runtime)
    static bool isLegacyLayout();     // LEGACY (旧第三方包)

    // 根据设置的 hermesRoot 相对路径解析 Hermes 内容根 (exe 所在目录 + 相对路径)。
    // "." / "./" 表示内容就在 exe 同级 (OFFICIAL 默认); 返回空表示未解析出。
    static fs::path resolveRoot(const std::string& hermesRootRelative);

    // 读取 settings.json 的 hermesRoot 并解析为绝对路径 (供插件 config_get 使用)
    static fs::path resolveRootFromSettings();

    // ---- 包内关键路径 ----
    static fs::path dataDir();        // <root>/data                     (纯 Hermes 数据目录)
    static fs::path sandboxDir();     // <root>/_home                    (HOME 劫持目标 / 宿主替身)
    static fs::path studioDir();      // <便携包根>/studio           (启动器自有存储, 不进 Agent 目录)
    static fs::path pluginDir();      // <便携包根>/plugin           (插件目录: <id>/plugin.dll)
    static fs::path envFile();        // <root>/data/.env                (渲染产物, Hermes 读)
    static fs::path configFile();     // <root>/data/config.yaml         (渲染产物, Hermes 读)
    static fs::path configTemplate(); // <root>/studio/config.template.yaml
    static fs::path logsDir();
    static fs::path backupDir();
    static fs::path downloadDir();

    // ---- 运行时可执行文件 (布局感知) ----
    static fs::path venvDir();        // <root>/python/venv | <root>/venv
    static fs::path venvPython();     // <venv>/Scripts/python.exe
    static fs::path hermesExe();      // <venv>/Scripts/hermes.exe | hermes.cmd
    static fs::path portablePython(); // <root>/python/base/python.exe | cpython-*/python.exe
    static fs::path nodeExe();        // <root>/node/node.exe | <root>/node/bin/node.exe
    static fs::path webUiEntry();     // <root>/webui/dist/server/index.js
    static fs::path desktopExe();     // <root>/desktop/**/*.exe (可能不存在)

    // ---- 目录/清单 (更新与构建用) ----
    static fs::path hermesAgentDir(); // <root>/python | <root>/hermes-agent
    static fs::path webUiDir();       // <root>/webui | <root>/node/node_modules/hermes-web-ui
    static fs::path pythonDir();      // <root>/python
    static fs::path nodeDir();        // <root>/node
    static fs::path gitDir();         // <root>/git

    // POSIX test(1) 垫片目录 (内含 [.cmd)。npm 生命周期脚本在 Windows 上用 cmd.exe
    // 执行, POSIX 风格 prepare (如 "[ -d dist ] || npm run build") 里的 "[" 找不到
    // 可执行文件 -> 误触发需要 devDependencies 的完整构建 -> npm install 必败。
    // 垫片实现 test 的 -d/-f/-e/-n/-z 常用形; 调用方把它前插到子进程 PATH。
    // 返回空 = 创建失败 (调用方按无垫片继续)。
    static fs::path ensurePosixTestShimDir();
    static fs::path runtimeManifestFile(); // <root>/runtime-manifest.json

    // 进程锁 / 状态文件 (Hermes 内核自己写的)
    static fs::path gatewayPidFile(); // data/gateway.pid
    static fs::path hermesLockFile(); // data/.hermes.lock

    // ===================== 沙箱 (便携化的核心) =====================
    //
    // 只劫持 HOME / USERPROFILE 是**不够**的 —— Windows 原生消费者无视 HOME:
    //   npm 把 cache 硬编码为 %LOCALAPPDATA%\npm-cache;
    //   Node 把编译缓存写 %TEMP%; uv/pip/playwright 各有自己的落点。
    // 实测证据见 docs/PORTABLE-LEAK-AUDIT.md。
    //
    // 这张表是**唯一真相源**: Launcher 起进程时按它构造环境块,
    // PortableBuilder 生成 Hermes.bat 时按它渲染 set 语句, 两处不会漂移。
    struct SandboxVar {
        const char* name;   // 环境变量名
        const char* rel;    // 相对 _home 的路径 ('/' 分隔; 空 = _home 本身)
        bool        isDir;  // true = 指过去之前必须先建出这个目录
    };
    static const std::vector<SandboxVar>& sandboxVarTable();

    // 按上表建出全部沙箱目录 (顺序关键: 目录先存在, 再有任何进程指过去;
    // 有些 runtime 在 %TEMP% 不存在时会直接失败)。
    static void createSandboxDirs();                        // 用当前已解析的 root
    static void createSandboxDirsAt(const fs::path& root);  // 指定 root (构建期用)

    // 把上表展开成 <名, 绝对路径> 列表 (不含 HERMES_* 那几个包特有变量)。
    static std::vector<std::pair<std::string, std::string>> sandboxEnvVars();
};

} // namespace hs
