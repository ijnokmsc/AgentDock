# HermesStudio 架构设计

> 目标：用 C++17 / Qt 6 Widgets 打造 Hermes 便携包启动器，替代并删除原配置中心（`lib/config_server.py`，:17520）。
> 上游：`https://github.com/yuluyangguang1/hermes-portable`，本地部署在 `I:\HermesPortable`（当前 v0.20.5 可运行）。

---

## 0. 决策基线（已确认）

| 决策项 | 结论 | 代价 / 备注 |
|---|---|---|
| GUI 技术栈 | **Qt 6.8.3 Widgets**（MSVC2022_64） | 需随包分发 Qt DLL（windeployqt 后约 25–40 MB） |
| Qt 获取 | aqtinstall 拉 qtbase+qttools 最小子集到 `thirdparty/Qt6` | 不装 Qt Creator / 不含 QML，体积最小 |
| 部署形态 | 源码在 `I:\HermesStudio`，**产物输出到 `I:\HermesPortable\` 根目录** | exe 与 `data/`、`hermes-agent/`、`node/` 同级，自身目录即 Hermes 根 |
| 更新策略 | **混合**：优先 Release 包（sha256 + 备份 + 回滚），失败回退 git pull / npm | 实现量中等，鲁棒性最好 |
| 配置真相源 | **启动器自有 JSON**，启动前渲染导出 `.env` + `config.yaml` | 单向：Web UI 内改的配置会被下次渲染覆盖（需在 UI 明确提示） |
| API Key 存储 | 明文 JSON（默认）+ **可选主密码**（PBKDF2-SHA256 + AES-GCM） | 换机即用；开主密码则每次启动输一次 |
| 更新范围 | 内核 / web-ui / **Python 运行时 / Node 运行时 / uv / 启动器自身** | 组件注册表驱动，各组件独立备份与回滚 |
| 网络 | 内置代理支持（System / Direct / Manual，含 SOCKS5 与 PAC） | 实测本机直连可用，但系统残留 `127.0.0.1:7897`，需可切换 |

### 实测环境（2026-09-19）

| 项 | 结果 |
|---|---|
| MSVC | `E:\炫彩IDE\data\VC\VC2022\14.41.34120`（cl/mt/rc 齐全，无 vcvarsall，需手动注入 INCLUDE/LIB） |
| Windows SDK | `E:\炫彩IDE\data\VC\Windows Kits\10\10.0.19041.0` |
| CMake / Ninja | 本机原先没有 → 由 venv 内的 PyPI 包提供 |
| 连通性 | api.github.com 200(0.64s) · registry.npmjs.org 200(0.51s) · nodejs.org 200(1.92s) |
| 系统代理 | `ProxyEnable=0`，`ProxyServer=127.0.0.1:7897`（未启用但已配置） |

---

## 1. 整体技术方案

### 1.1 定位

`HermesStudio.exe` 是便携包的**宿主外壳**，取代 `Hermes.bat` + `config_server.py` 两个东西：

- 对**用户**：一个窗口完成「体检 → 配模型 → 更新 → 启动」
- 对**Hermes**：只负责把 `data/.env` 和 `data/config.yaml` 渲染到位，然后拉起进程（CLI / WebUI / Desktop）

Hermes 内核本身**零改动**——这是本方案最重要的约束。所有改造都在外层，内核升级不会被打回。

### 1.2 分层

```
┌─────────────────────────────────────────────────────┐
│  UI 层 (Qt Widgets)      MainWindow / Pages / Models │  ← 可替换，不涉业务逻辑
├─────────────────────────────────────────────────────┤
│  Core 层 (纯 C++17，无 Qt 依赖，可单测)              │
│  Preflight · Launcher · Updater · ModelRegistry      │
│  ConfigRenderer · ComponentRegistry · Version         │
├─────────────────────────────────────────────────────┤
│  Platform 层 (Win32)                                 │
│  WinHTTP(含代理) · BCrypt(sha256/AES-GCM) · miniz    │
│  CreateProcess · 端口扫描 · 注册表(代理)             │
└─────────────────────────────────────────────────────┘
```

**依赖倒置**：Core 只依赖纯虚接口（`IHttpClient` / `IProcessRunner` / `IFileSystem` / `IClock`），Platform 提供实现并在 `main()` 注入。这样 Core 可以在没有 Qt、没有网络的环境下跑单元测试。

**并发模型**：单进程。UI 主线程只做渲染与事件；下载、解压、进程等待、版本探测全部丢到 `QThreadPool`，通过信号回主线程。禁止在 UI 线程做同步网络/文件 IO。

**数据流**：

```
models.json ──┐
              ├─ ConfigRenderer ─→ data/.env ──┐
settings.json ┘                  data/config.yaml ─┴─→ Launcher ─→ hermes / node
```

### 1.3 依赖清单

| 依赖 | 版本 | 方式 | 理由 |
|---|---|---|---|
| Qt | 6.8.3 | 动态 DLL，windeployqt 部署 | LGPL，LGPL 合规需随附可重链说明 |
| nlohmann/json | 3.11.3 | vendored 单头 | 已下载到 `thirdparty/nlohmann/json.hpp` |
| miniz | 3.x | vendored 单头 | 解压 Release zip；公共领域，无许可负担 |
| WinHTTP | 系统 | 链接 `winhttp.lib` | 零分发成本，原生支持系统代理/PAC/企业证书 |
| BCrypt (CNG) | 系统 | 链接 `bcrypt.lib` | SHA-256 与 AES-GCM 均可用 |
| **不引入** | yaml-cpp / SQLite / libcurl / OpenSSL | — | 见 §4.4 说明 |

---

## 2. 模块划分与职责

| 模块 | 目录 | 职责 | 关键类 |
|---|---|---|---|
| 应用骨架 | `src/app` | 单实例守卫、路径解析、全局设置、日志初始化 | `Application`, `Paths`, `Settings`, `SingleInstanceGuard` |
| 领域模型 | `src/core/domain` | Provider / Model / RuntimeConfig / Component 的值对象与校验 | `Provider`, `Model`, `RuntimeConfig`, `ComponentId` |
| 模型仓库 | `src/core/registry` | providers + models 的 CRUD、默认模型、schema 迁移 | `ModelRegistry` |
| 配置渲染 | `src/core/render` | JSON → `.env` / `config.yaml`（含模板合并、原子写、备份） | `ConfigRenderer`, `YamlEmitter`, `EnvWriter` |
| 前置检查 | `src/core/preflight` | 可插拔检查项，产出带修复动作的报告 | `PreflightChecker`, `CheckItem`, `CheckResult` |
| 启动器 | `src/core/launch` | 三种模式拉起、环境注入、进程监管、优雅停止 | `Launcher`, `ProcessSupervisor`, `StartMode` |
| 更新中心 | `src/core/update` | 组件注册、版本比对、计划编排、备份、应用、回滚 | `ComponentRegistry`, `UpdatePlanner`, `Updater`, `BackupManager` |
| 更新源适配器 | `src/core/update/sources` | 各组件的远端版本探测与下载地址解析 | `GitHubReleaseSource`, `NpmSource`, `NodeDistSource`, `PythonBuildStandaloneSource`, `StaticManifestSource` |
| 版本比较 | `src/core/update` | SemVer + 日期版本（python-build-standalone 的 `20250909`） | `VersionComparator` |
| 平台抽象 | `src/platform` | HTTP(代理)、哈希、加解密、解压、进程、端口、注册表 | `WinHttpClient`, `Hash`, `Crypto`, `ZipReader`, `WinProcess`, `PortScanner`, `ProxyConfig` |
| 存储 | `src/storage` | 原子 JSON 读写、JSONL 追加日志、目录备份 | `JsonStore`, `JsonlLog`, `Backup`, `AtomicFile` |
| UI | `src/ui` | 主窗口、页面、表格模型、异步任务桥 | `MainWindow`, `HomePage`, `DiagnosticsPage`, `ModelsPage`, `UpdatesPage`, `SettingsPage`, `LogsPage` |

**边界规则**：`src/core/**` 不得 `#include <Qt*>`；`src/ui/**` 不得直接调 `src/platform/**`（必须经 Core）。

---

## 3. 项目目录结构

```
I:\HermesStudio\
├── CMakeLists.txt                    # 顶层：C++17、Qt6、MSVC 选项、子目录
├── cmake/
│   ├── Qt6Config.cmake               # 优先 thirdparty/Qt6，回退系统 find_package
│   └── MsvcEnv.cmake                 # 注入炫彩 VC 的 INCLUDE/LIB/PATH
├── scripts/
│   ├── env_msvc.bat                  # 手工构造 MSVC 环境（无 vcvarsall）
│   ├── bootstrap_qt.py               # aqtinstall 拉 Qt（含镜像回退）
│   ├── build.bat                     # 配置 + 编译
│   └── deploy.bat                    # windeployqt + 拷贝到 I:\HermesPortable
├── src/
│   ├── main.cpp
│   ├── app/
│   │   ├── Application.h/.cpp        # QApplication 子类、初始化顺序
│   │   ├── Paths.h/.cpp              # HermesRoot / DataDir / StudioDir / 运行时路径
│   │   ├── Settings.h/.cpp           # settings.json 读写 + 变更信号
│   │   └── SingleInstanceGuard.h/.cpp
│   ├── core/
│   │   ├── domain/
│   │   │   ├── Provider.h/.cpp
│   │   │   ├── Model.h/.cpp
│   │   │   ├── RuntimeConfig.h/.cpp
│   │   │   └── Types.h               # 枚举、Result<T>、错误码
│   │   ├── registry/ModelRegistry.h/.cpp
│   │   ├── render/
│   │   │   ├── ConfigRenderer.h/.cpp
│   │   │   └── YamlEmitter.h/.cpp    # 极简 YAML 子集写入（不解析）
│   │   ├── preflight/
│   │   │   ├── PreflightChecker.h/.cpp
│   │   │   └── checks/               # RootLayoutCheck, RuntimeCheck, ConfigCheck,
│   │   │                             # ModelCheck, PortCheck, EditablePathCheck, DiskSpaceCheck
│   │   ├── launch/
│   │   │   ├── Launcher.h/.cpp
│   │   │   └── ProcessSupervisor.h/.cpp
│   │   ├── update/
│   │   │   ├── Component.h/.cpp      # 组件定义与本地版本探测
│   │   │   ├── ComponentRegistry.h/.cpp
│   │   │   ├── UpdatePlanner.h/.cpp  # 生成计划 + 依赖排序 + 空间预估
│   │   │   ├── Updater.h/.cpp        # 状态机驱动
│   │   │   ├── BackupManager.h/.cpp
│   │   │   ├── VersionComparator.h/.cpp
│   │   │   └── sources/*.h/.cpp
│   │   └── port/                     # 纯虚接口：IHttpClient/IProcess/IFileSystem/IClock
│   ├── platform/
│   │   ├── WinHttpClient.h/.cpp      # WinHTTP + 代理(SYSTEM/DIRECT/MANUAL+PAC) + 进度回调
│   │   ├── Hash.h/.cpp               # BCrypt SHA-256（流式）
│   │   ├── Crypto.h/.cpp             # PBKDF2 + AES-GCM（Key 加密）
│   │   ├── ZipReader.h/.cpp          # miniz 封装
│   │   ├── WinProcess.h/.cpp         # CreateProcess + 作业对象 + 输出泵
│   │   ├── PortScanner.h/.cpp        # 端口占用与占用进程
│   │   └── ProxyConfig.h/.cpp        # 读 IE/系统代理设置
│   ├── storage/
│   │   ├── JsonStore.h/.cpp          # 原子写 + schemaVersion 迁移钩子
│   │   ├── JsonlLog.h/.cpp
│   │   ├── Backup.h/.cpp
│   │   └── AtomicFile.h/.cpp
│   └── ui/
│       ├── MainWindow.h/.cpp         # 左侧导航 + QStackedWidget
│       ├── HomePage.h/.cpp           # 状态卡片 + 启动/停止 + 模式选择
│       ├── DiagnosticsPage.h/.cpp    # 检查项列表 + 一键修复
│       ├── ModelsPage.h/.cpp         # Provider/Model 表格 + 参数表单 + 连接测试
│       ├── UpdatesPage.h/.cpp        # 组件卡片 + 计划勾选 + 进度 + 历史/回滚
│       ├── SettingsPage.h/.cpp       # 路径/端口/代理/主密码/更新通道
│       ├── LogsPage.h/.cpp
│       ├── models/                   # QAbstractTableModel 适配器
│       └── widgets/                  # StatusBadge, ProgressRow, KeyInput
├── thirdparty/
│   ├── nlohmann/json.hpp             # ✅ 已下载
│   ├── miniz/                        # miniz.c + miniz.h
│   └── Qt6/                          # aqtinstall 输出（构建期，不入 git）
├── resources/
│   ├── icons/                        # 复用 I:\HermesPortable\icons 的 SVG
│   ├── app.rc                        # 版本信息 + 图标
│   └── styles/app.qss
├── tests/
│   ├── CMakeLists.txt
│   └── core/                         # VersionComparatorTest, ConfigRendererTest,
│                                     # ModelRegistryTest, UpdatePlannerTest
├── build/                            # CMake 构建目录（不入 git）
└── docs/
    ├── ARCHITECTURE.md               # 本文
    ├── SCHEMA.md                     # JSON 存储结构定义
    └── MIGRATION.md                  # 迁移与回滚操作手册
```

---

## 4. 关键数据结构与存储方案

### 4.1 核心数据结构（C++）

```cpp
// src/core/domain/Types.h
enum class ProviderKind { OpenAI, Anthropic, OpenAICompatible, Ollama };
enum class StartMode   { Cli, WebUI, Desktop };
enum class ComponentId { Kernel, WebUI, PythonRuntime, NodeRuntime, Uv, Launcher };

template <class T> struct Result {           // 不用异常做流程控制
    std::optional<T> value;
    std::optional<Error> error;              // {code, message, detail}
    explicit operator bool() const { return value.has_value(); }
};

struct Provider {
    QString id;                 // "deepseek"
    QString displayName;        // "DeepSeek"
    ProviderKind kind = ProviderKind::OpenAICompatible;
    QString baseUrl;            // "https://api.deepseek.com/v1"
    QString envVar;             // "DEEPSEEK_API_KEY"
    QString apiKey;             // 明文或密文（见 cipher 字段）
    bool    encrypted = false;
    bool    enabled   = true;
    QString iconName;           // 对应 icons/*.svg
};

struct ModelParams {
    std::optional<double> temperature, topP;
    std::optional<int>    maxTokens, contextWindow, maxOutputTokens;
    std::optional<int>    timeoutSec;
    QVariantMap           extra;              // 透传给 config.yaml 的可选字段
};

struct Model {
    QString id;                 // 本地唯一标识 "deepseek/deepseek-v4-flash-0731"
    QString providerId;
    QString upstreamId;         // 真正发给上游的 model 名
    QString displayName;
    ModelParams params;
    bool  supportsTools = true;
    bool  enabled  = true;
    bool  isDefault = false;    // 全表唯一
    QString addedAt;
};

struct RuntimeConfig {
    QString hermesRoot;                        // I:\HermesPortable
    QString pythonExe, uvExe, nodeExe;         // 解析后的绝对路径
    int  webUiPort = 8648;
    int  gatewayPort = 0;                      // 0 = 由 Hermes 自定
    StartMode startMode = StartMode::WebUI;
    bool autoCheckUpdate = true;
    QString updateChannel = "stable";          // stable | beta
    bool launchOnStart = false;
};

struct NetSettings {
    enum class Mode { System, Direct, Manual } mode = Mode::System;
    QString host; int port = 0;
    enum class Type { Http, Socks5 } type = Type::Http;
    QString username, password, bypassList;
    int  timeoutSec = 30, retry = 2;
    bool allowInsecureTls = false;             // 企业自签名证书场景
    QString githubMirror;                      // 可选：ghproxy 类加速前缀
};

// 更新相关
struct ComponentState {
    ComponentId id;
    QString currentVersion, latestVersion;
    QString installPath;
    qint64  lastCheckTs = 0;
    bool    updateAvailable = false;
    QString channel;
};
struct UpdatePlanItem {
    ComponentId id; QString fromVersion, toVersion;
    QUrl downloadUrl; QString sha256; qint64 sizeBytes = 0;
    bool requiresStop = true;                  // 更新前是否须停 Hermes
    QString strategy;                          // "replace_dir" | "replace_files" | "npm" | "git"
};
```

### 4.2 本地存储布局（全部在便携包内，跟随 U 盘）

```
I:\HermesPortable\data\studio\
├── settings.json        # RuntimeConfig + NetSettings + UI 偏好      (schemaVersion=1)
├── models.json          # providers[] + models[]                     (schemaVersion=1)
├── updates.json         # 各组件版本快照、通道、上次检查、自动更新开关
├── history.jsonl        # 追加写入的更新事件（ts, component, from, to, result）
├── logs\studio-YYYY-MM-DD.log
├── backups\<component>-<YYYYmmdd_HHMMSS>\   # 更新前备份，保留最近 N=3 份
├── cache\downloads\                          # 下载暂存，成功后清理
└── config.template.yaml                      # config.yaml 的 mcp_servers 等固定段落
```

**渲染产物**（Core 写、Hermes 读，每次启动前刷新，写前先 `.bak`）：
- `data\.env` ← 所有 `enabled` Provider 的 `ENV_VAR=key`
- `data\config.yaml` ← `model:`/`agent:`/`terminal:`/`compression:`/`display:`/`memory:` + 模板里的 `mcp_servers:` 原样附加

### 4.3 JSON 示例

```jsonc
// models.json
{
  "schemaVersion": 1,
  "providers": [
    { "id": "deepseek", "displayName": "DeepSeek", "kind": "openai-compatible",
      "baseUrl": "https://api.deepseek.com/v1", "envVar": "DEEPSEEK_API_KEY",
      "apiKey": "sk-...", "encrypted": false, "enabled": true, "iconName": "deepseek" }
  ],
  "models": [
    { "id": "deepseek/deepseek-v4-flash-0731", "providerId": "deepseek",
      "upstreamId": "deepseek-v4-flash-0731", "displayName": "DeepSeek V4 Flash",
      "params": { "temperature": 0.3, "maxTokens": 8192, "contextWindow": 131072 },
      "supportsTools": true, "enabled": true, "isDefault": true,
      "addedAt": "2026-09-19T04:00:00+08:00" }
  ]
}
```

**原子写协议**（`AtomicFile`）：写 `xxx.json.tmp` → `FlushFileBuffers` → `MoveFileEx(MOVEFILE_REPLACE_EXISTING)` → 成功后才删旧 `.bak`。断电/强杀只会留下 `.tmp`，主文件始终完整。

**Schema 迁移**：每个存储文件带 `schemaVersion`，`JsonStore::load()` 遇到旧版本时按注册的迁移函数链逐级升级，升级前自动备份一份 `*.v<N>.bak`。

### 4.4 为什么不用 SQLite（以及何时该换）

- 数据量级：Provider ≤ 100、Model ≤ 1000、更新历史 ≤ 数千条。SQLite 的查询/索引优势用不上。
- 便携性：JSON 可直接文本查看、手改、拷走、git diff；SQLite 二进制文件在 U 盘异常拔出时更容易损坏。
- 依赖成本：SQLite 虽是单文件，但仍要新增编译单元与使用约定。

**预留接口**：`storage/IStore.h` 定义 `load/save/appendHistory/queryHistory`，当前实现 `JsonStore`；若将来需要复杂审计查询或配置量上量级，替换为 `SqliteStore` 时 Core 层无需改动。

**同理不引入 yaml-cpp**：启动器对 `config.yaml` 只写不读（模板段落以纯文本附加），一个 80 行的缩进发射器足够；引入完整 YAML 解析器属于过度工程。

---

## 5. 状态流转

### 5.1 启动流程

```
        ┌─────────┐
        │  Idle   │
        └────┬────┘
             │ 用户点「启动」（或 launchOnStart）
             ▼
      ┌──────────────┐   失败且不可自动修复
      │  Preflight   │──────────────────────► Blocked（跳转体检页，高亮失败项）
      └──────┬───────┘
             │ 全部通过 / 已自动修复
             ▼
      ┌──────────────┐
      │   Rendering  │  渲染 .env / config.yaml（原子写 + .bak）
      └──────┬───────┘
             ▼
      ┌──────────────┐
      │  Launching   │  注入 HERMES_HOME 等环境 → CreateProcess
      └──────┬───────┘
             ▼
   ┌──────────────────┐   进程退出码非 0 / 端口未起
   │     Running      │──────────────────────► Failed（展示最后 200 行日志）
   └──────┬───────────┘
          │ 用户点「停止」/ 关窗口
          ▼
      ┌──────────┐   Ctrl+Break → 等待 5s → Terminate → 清 lock/pid
      │ Stopping │
      └────┬─────┘
           ▼
       Stopped → Idle
```

**Preflight 检查项**（每项产出 `Pass/Warn/Fail` + 可选 `Remediation`）：

| # | 检查 | 失败后果 | 修复动作 |
|---|---|---|---|
| 1 | 目录布局（`data/`、`hermes-agent/`、`venv|python/`、`node/`） | Fail | 提示重定位根目录 |
| 2 | 运行时可执行文件（`python.exe`、`uv.exe`、`node.exe`） | Fail | 自动探测常见路径并写回 settings |
| 3 | 配置完整性（`models.json` 可解析、schema 匹配） | Fail | 从 `.bak` 恢复 / 重建空配置 |
| 4 | 至少 1 个 enabled provider 且 Key 非空 | Fail | 跳转模型页 |
| 5 | 恰好 1 个 default model | Fail | 自动选第一个可用模型 |
| 6 | 端口 8648 空闲；17520 无残留配置中心 | Warn/Fail | 列出占用进程 PID，一键结束 |
| 7 | editable 路径（`__editable___hermes_agent_*_finder.py` 指向当前根目录） | Warn | 调 `lib/fix_editable_paths.py` |
| 8 | 磁盘可用空间 ≥ 预估（更新时才校验） | Fail | 阻止更新 |
| 9 | 残留 `gateway.lock` / `gateway.pid` 指向已死进程 | Warn | 清理 |

第 7 项是历史坑位（见 `I:\HermesPortable\.workbuddy\memory\2026-09-19.md`）：便携包移动后 editable install 的绝对路径会失效，导致 `No module named 'hermes_cli'`。启动器必须内置此检查并自动修复。

### 5.2 更新流程

```
   Idle
    │ 手动「检查更新」/ 启动时的自动检查（受 autoCheckUpdate 控制）
    ▼
 Checking ──网络失败──► CheckFailed（记录错误，可用代理设置重试）
    │
    ├─ 无新版本 ──► UpToDate
    └─ 有新版本 ──► PlanReady（展示组件清单与体积，用户勾选）
                       │
                       ▼
                  Downloading（进度回调：已收/总量、速率）
                       │
                       ▼
                  Verifying（SHA-256 比对；不符 → 删除重下，最多 retry 次）
                       │
                       ▼
                  Stopping（若 requiresStop：停 Hermes 与 node 进程）
                       │
                       ▼
                  BackingUp（按组件策略备份到 backups/）
                       │
                       ▼
                  Applying（解压 / 覆盖 / npm / git pull）
                       │
                       ▼
                  PostCheck（版本探测 + 自检，如 venv 是否仍可用）
                       │
          ┌────────────┴────────────┐
          ▼                         ▼
      Succeeded                  Failed
          │                         │
          ▼                         ▼
   清理旧备份(保留N份)         RollingBack ──► RolledBack / RollbackFailed(需手动)
```

**事务粒度**：以**单个组件**为事务单位（每个组件独立备份/应用/回滚）。多组件批量更新按顺序执行，某组件失败不自动回滚已成功的组件，但会在报告中明确列出「已更新 X、失败 Y」，并允许单独回滚。

### 5.3 组件更新矩阵

| 组件 | 本地版本探测 | 远端源 | 下载物 | 应用策略 | 备份范围 | 风险与对策 |
|---|---|---|---|---|---|---|
| **Kernel**<br>`hermes-agent/` | `hermes --version` 输出解析；回退 `VERSION` / git HEAD | ① GitHub Release(NousResearch/hermes-agent) ② git commits API | zip / git pull | 覆盖目录（排除 `.git`、`__pycache__`），随后跑 `fix_editable_paths.py` | `hermes-agent/`（排除 `.git`） | 更新后 editable 路径失效 → 必须重跑修复脚本 |
| **WebUI**<br>`hermes-web-ui` | `node/node_modules/hermes-web-ui/package.json` 的 `version` | npm registry `ekko-studio` | `.tgz` | 替换 `bin/`+`dist/`+`package.json`，**保留包内 `node_modules/`** | 该包目录 | 整目录删除会连带删掉依赖 → 只替换包自身文件 |
| **Python 运行时**<br>`python/` | `python/python.exe --version` | astral-sh/python-build-standalone Release tag | `install_only` tar.gz | 解压替换 `python/` | `python/` | `venv/pyvenv.cfg` 的 `home=` 指向 `python/`：跨小版本替换会让 venv 失效 → 默认标记为**高风险**，更新后强制自检 `venv\Scripts\python.exe --version`，失败则提示重建 venv |
| **Node 运行时**<br>`node/` | `node/node.exe --version` | nodejs.org/dist（或 GitHub nodejs/node） | win-x64 zip | 只替换 `node.exe` + 核心 dll，**绝不动 `node/node_modules/`** | `node/*.exe|*.dll` | `node_modules` 里装着 web-ui 与全部依赖（数百 MB），误删等于删掉 web-ui |
| **uv** | `uv.exe --version` | astral-sh/uv Release | zip | 替换单文件 `uv.exe` | 单文件 | 低风险 |
| **Launcher 自身** | 编译期常量 | 自建 Release | zip / exe | 下载为 `HermesStudio.exe.new`，退出时由自替换 bat 完成 | exe 本体 | 需作业对象/重启替换；保留 `.old` 供回滚 |

**更新前通用前置**：停 Hermes 相关进程（否则文件被占用，Windows 无法替换）→ 磁盘空间校验（备份 + 下载包 ≥ 1.5×组件体积）→ 写 `history.jsonl` 起始事件。

### 5.4 网络与代理

`NetSettings::Mode` 三态映射到 WinHTTP：

| 模式 | WinHTTP 实现 |
|---|---|
| `System`（默认） | `WinHttpOpen(WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY)` + `WinHttpGetIEProxyConfigForCurrentUser` 兜底；PAC 场景用 `WinHttpGetProxyForUrl` |
| `Direct` | `WINHTTP_ACCESS_TYPE_NO_PROXY` |
| `Manual` | `WINHTTP_ACCESS_TYPE_NAMED_PROXY`，`http=`/`socks=` 前缀；`WinHttpSetOption(WINHTTP_OPTION_PROXY_USERNAME/PASSWORD)` |

- 所有请求走**统一入口** `WinHttpClient`，组件源只负责拼 URL 与解析响应 → 代理/TLS/重试对上层透明。
- `allowInsecureTls` 关闭证书校验（企业网络自签名），默认关。
- `githubMirror` 允许用户填加速前缀（直连实测可用，但保留后路）。
- 设置页提供**「测试连接」**：并发测 GitHub API / npm / nodejs.org，返回各自耗时与错误码。

### 5.5 运行模式与进程监管

启动器要能**启动也要能停止** Hermes 与 Web UI，共三种模式：

| 模式 | 进程 | 启动方式 | 定位方式（用于停止） |
|---|---|---|---|
| **CLI** | `venv\Scripts\hermes.exe` | `CREATE_NEW_CONSOLE` 独立控制台 | 记录 PID；兜底读 `data/gateway.pid` |
| **WebUI** | `node node_modules\hermes-web-ui\dist\server\index.js start 8648` | `CREATE_NO_WINDOW` 后台（原 bat 用 PowerShell 分离启动，启动器改为直接 CreateProcess） | 记录 PID；兜底按 8648 端口反查 |
| **Desktop** | `runtime\desktop\**\*.exe`（Electron） | 常规启动 | 记录 PID |

> **实测**：当前便携包**没有** `runtime\` 目录，即不含桌面版运行时。因此 Desktop 模式必须做存在性检测（`Launcher::modeAvailable()`），不可用时在 UI 置灰并给出明确理由，与原「配置中心」的行为保持一致，不能静默失败。

**停止顺序**（反向依赖，避免子进程变孤儿）：

```
Desktop  →  内核 (gateway.pid)  →  Web UI (端口反查)  →  清理残留锁
```

- **优雅优先**：`AttachConsole` + `CTRL_BREAK_EVENT`，给进程走清理流程的机会；超时（默认 5s）后 `killTree` 强杀。
- **进程树**：node / gateway 会 spawn 子进程，用 `CreateToolhelp32Snapshot` 递归枚举子进程，先杀子再杀父，避免残留。
- **锁清理**：停止后删除 `gateway.pid`、`gateway_state.json`、`.hermes.lock`——这三个文件若指向已死进程，下次启动的体检会报「残留进程锁」（Preflight 第 9 项）。
- **端口占用复用**：WebUI 启动时若 8648 已被占用，不重复拉起，直接复用并提示 PID（避免起两个实例抢端口）。

---

## 6. UI 界面结构

`QMainWindow`：左侧 96px 图标导航栏 + 右侧 `QStackedWidget` + 底部全局状态条（Hermes 运行状态 / 版本 / 更新红点）。

| 页面 | 主要控件 | 交互要点 |
|---|---|---|
| **概览 Home** | 状态卡片（内核版本、WebUI 版本、默认模型、Hermes 状态）、模式单选（CLI / WebUI / Desktop）、大按钮「启动 / 停止」、最近日志 10 行 | 未通过体检时「启动」置灰并显示原因；启动后按钮变停止，实时 tail 日志 |
| **体检 Diagnostics** | 检查项列表（图标 + 名称 + 详情 + 「修复」按钮）、顶部「重新检查」、汇总条 | 一键修复全部可修复项；不可修复项给出明确指引（跳转/改设置） |
| **模型配置 Models** | 左：Provider 列表（新增/编辑/删除/启停）；右：该 Provider 下的模型表格；下方：参数表单（temperature / maxTokens / contextWindow / timeout / extra）；底部：「测试连接」「设为默认」 | Key 输入框默认掩码 + 眼睛切换；测试连接走后台线程直接打 provider 的 `/models`；默认模型唯一性校验 |
| **更新 Updates** | 组件卡片网格（图标、当前版本、最新版本、状态徽标）、「检查更新」、勾选框生成计划、进度条（下载/校验/应用）、右侧「历史与回滚」列表 | 批量更新按依赖排序（Kernel 先于 WebUI）；高风险组件（Python 运行时）需二次确认；失败项单独「回滚」 |
| **设置 Settings** | 分组：路径（Hermes 根、各运行时）、端口（WebUI / Gateway）、更新（自动检查、通道 stable/beta、备份保留数）、网络（代理三态 + 测试连接）、安全（主密码开关/修改）、外观（主题/语言） | 主密码开启时对全部明文 Key 做一次加密迁移，并提示「忘记密码无法恢复」 |
| **日志 Logs** | 分级过滤（Debug/Info/Warn/Error）、搜索、导出、打开日志目录 | 同时显示启动器自身日志与 Hermes 输出 |

**模型层**：`ProviderTableModel` / `ModelTableModel` 继承 `QAbstractTableModel`，数据源是 `ModelRegistry` 的内存快照，编辑经 `ModelRegistry` 落盘后再刷新——UI 不直接写 JSON。

**响应式**：窗口 960×640 起步，可最小化到托盘；关闭窗口时若 Hermes 在运行，弹「最小化到托盘 / 停止并退出 / 后台继续」三选一。

---

## 7. 与 I:\HermesPortable 的对接与迁移

### 7.1 根目录定位

`Paths::hermesRoot()` 解析优先级：
1. 命令行 `--root <dir>`
2. `settings.json` 里显式配置且目录合法
3. **可执行文件所在目录**（部署形态 A 的主路径）
4. 逐层向上找含 `data/` + `hermes-agent/` 的祖先目录（兜底）

命中后校验「指纹三件套」：`data\`、`hermes-agent\`、`(venv\ | python\)`，缺任一即判为无效根目录并提示。

### 7.2 需保留（不得删除）

| 路径 | 原因 |
|---|---|
| `data/`（全部，含 `.env`、`config.yaml`、`sessions/`、`memories/`、`kanban.db`、`skills/`、`logs/`） | 用户数据主权区，删除不可逆 |
| `hermes-agent/`（含 `.git`） | 内核源码 + git 回退更新通道 |
| `venv/`、`python/` | Python 运行时与已装依赖 |
| `node/`（**含 `node_modules/`**） | Node 运行时 + hermes-web-ui 及依赖 |
| `uv.exe` | 依赖安装器，更新与重建 venv 都要用 |
| `_home/` | 沙箱 HOME，`.hermes` 软链指向 `data/` |
| `lib/fix_editable_paths.py` | 移动目录后修复 editable 绝对路径的关键脚本 |
| `lib/fix_shims.py` | 启动 shim 修复 |
| `lib/chat_viewer.py` | 独立功能，与配置中心无关 |
| `lib/update.py` | **保留作为回退通道**（git/npm 分支调用它），不删 |
| `tools/build.py` | 构建上游包用 |
| `icons/`、`fonts/` | 启动器直接复用这些品牌 SVG 与字体 |
| `Hermes.bat` | **降级为兜底脚本**（去掉启动配置中心的段落），Qt 出问题时仍能起 Hermes |
| `packages/server/` | 现有 server 包数据 |

### 7.3 可删除（确认配置中心退役后）

| 路径 | 说明 |
|---|---|
| `lib/config_server.py`（147 KB） | 配置中心后端，本项目的替换目标 |
| `lib/config/`（`index.html` + `index-standalone.html`） | 配置中心前端 |
| `data/runtime.json` | 仅记录配置中心的 port/token/pid，退役后无意义 |
| `data/.update_check` | 旧版更新检查缓存，改由 `data/studio/updates.json` 承担 |
| `HermesPortable使用说明.html` | 需重写（启动器用法变了） |

> **建议**：先重命名为 `lib/_deprecated_config_center/` 观察一个版本周期，确认无回归后再物理删除。删除窗口期同样保留 `Hermes.bat` 兜底。

### 7.4 新增

| 路径 | 说明 |
|---|---|
| `HermesStudio.exe` + `Qt6Core.dll`、`Qt6Gui.dll`、`Qt6Widgets.dll`、`platforms/qwindows.dll`、`styles/` 等 | windeployqt 产出，约 25–40 MB |
| `data/studio/` | 见 §4.2 |
| `data/config.template.yaml` | 首次迁移时从现有 `config.yaml` 抽取 `mcp_servers:` 段落生成 |
| `HermesStudio.lnk`（可选） | 替换原 `Hermes.bat` 的用户入口 |

### 7.5 迁移步骤（按顺序）

1. **备份**：整包拷贝 `I:\HermesPortable` → `I:\HermesPortable.bak-<日期>`（更新与迁移的通用前置）。
2. **停止服务**：结束 Hermes CLI、node（web-ui）、以及 :17520 的配置中心进程。
3. **首次运行启动器** → 触发迁移向导：
   - 解析现有 `data/.env` → 生成 `providers[]`（按 `*_API_KEY` 推断 provider 名与 envVar；`CUSTOM_BASE_URL` 归入 custom provider）
   - 解析现有 `data/config.yaml` 的 `model:` 段 → 生成 `models[]` 首条并置为 default
   - 抽取 `config.yaml` 的 `mcp_servers:` 段 → 落成 `data/studio/config.template.yaml`
   - 其余 `agent/terminal/compression/display/memory` 段 → 存入 `settings.json` 的 `renderOverrides`
   - 写 `data/studio/{settings,models,updates}.json`（`schemaVersion=1`）
4. **双向验证**：启动器渲染出的 `.env`/`config.yaml` 与迁移前文件做逐行 diff（忽略顺序），有差异则在迁移报告里列出，确认无误后才允许继续。
5. **启动验证**：用启动器拉起 CLI 与 WebUI 各一次，确认 `hermes --version` 正常、:8648 可访问。
6. **退役配置中心**：重命名 `lib/config_server.py` 与 `lib/config/` 到 `_deprecated/`，同时把 `Hermes.bat` 里启动配置中心的段落注释掉。
7. **观察一个版本周期**，无回归后按 §7.3 清理。

> 迁移全程可逆：第 1 步的整包备份 + 渲染前的 `.bak`，任一环节出问题都能回到原状态。

---

## 8. 最小可运行版本（MVR）实施顺序

| 里程碑 | 内容 | 完成判据 |
|---|---|---|
| **M0 骨架** | CMake 工程 + MSVC 环境脚本 + Qt 接入 + 空主窗口 + `Paths` | `HermesStudio.exe` 能启动并显示窗口，标题栏显示解析到的 Hermes 根目录 |
| **M1 体检 + 启动** | `PreflightChecker`（前 7 项）、`Launcher`（CLI/WebUI）、进程监管、日志页 | 能完全替代 `Hermes.bat`：双击 exe → 体检 → 启动 WebUI，浏览器打开 :8648 可用 |
| **M2 模型配置** | `ModelRegistry` + `ModelsPage` + `ConfigRenderer`（`.env` + `config.yaml`） + 迁移向导 | 增删改模型后渲染出的配置文件与手工编辑等效；**此时即可退役配置中心** |
| **M3 更新中心** | `ComponentRegistry` + 版本比对 + Release 下载 + SHA-256 + 备份 + 应用 + 回滚；Kernel/WebUI 先落地，Python/Node/uv 随后 | 断网注入假版本能走通「有更新→下载→校验→备份→应用→回滚」全链路 |
| **M4 打磨** | 代理设置与测试连接、主密码加密、更新通道/自动检查、托盘、打包脚本、安装说明 | 全流程无阻塞；产出 `deploy.bat` 一键部署到便携包 |

**当前进度（2026-09-19）**

| 里程碑 | 状态 | 说明 |
|---|---|---|
| M0 骨架 | ✅ 完成 | CMake + MSVC14.41 + Qt 6.8.3 接入，`HermesStudio.exe` 编译通过并部署到 `I:\HermesPortable` |
| M1 体检+启动 | 🟡 主体完成 | 7 项体检、三种模式拉起/停止、首次运行迁移均已实测通过；日志页已实现，托盘已加 |
| M2 模型配置 | 🟡 主体完成 | Provider/Model 增删改 + 渲染 `.env`/`config.yaml` 已实现，尚未做「连接测试」 |
| M3 更新中心 | 🟡 主体完成 | WinHTTP 网络层 + miniz 解压 + checkRemote 版本比对 + UpdateManager 备份/替换/回滚已实现；内核/web-ui 更新流程已接 UI，待实机测试 |
| M4 打磨 | 🟡 主体完成 | 设置页(代理/端口/通道/路径)、日志页(过滤/搜索/导出)、托盘、deploy.bat 已实现；主密码加密待补 |

已实测通过的行为：双击 exe → 自动定位 `I:\HermesPortable` → 从 `.env`/`config.yaml` 迁移出 4 个 Provider 与默认模型 → 生成 `data/studio/{models.json,config.template.yaml}` → 探测各组件本地版本。**原 `.env` 未被改动**（渲染只在用户点「启动」时发生）。

**建议节奏**：M0–M2 是「能替代」的最小闭环，优先做完即可删配置中心；M3 分批上组件（Kernel → WebUI → uv → Node → Python 运行时），风险由低到高；Python 运行时更新因涉及 venv 有效性，放最后并强制二次确认。

---

## 9. 风险登记

| # | 风险 | 影响 | 对策 |
|---|---|---|---|
| R1 | 渲染覆盖用户在 Web UI 里的改动 | 配置悄悄丢失 | 渲染前若检测到 `.env`/`config.yaml` 的 mtime 新于上次渲染，弹「外部已修改，是否以启动器配置覆盖？」并存 diff |
| R2 | 更新 Node 运行时误删 `node_modules` | web-ui 整个消失 | 白名单替换（仅 `node.exe` + 核心 dll），单元测试覆盖「更新后 `node_modules/hermes-web-ui/package.json` 仍存在」 |
| R3 | 更新 Python 运行时导致 venv 失效 | Hermes 起不来 | 标记为高风险 + 更新后自检 `venv\Scripts\python.exe --version`，失败自动回滚 |
| R4 | portable 目录移动后 editable 路径失效 | `No module named 'hermes_cli'` | Preflight 第 7 项，自动跑 `fix_editable_paths.py` |
| R5 | 国内网络访问 GitHub/npm 不稳 | 更新失败 | 代理三态 + 镜像配置 + 测试连接 + 重试与明确错误码（区分 403 限流 / DNS / TLS） |
| R6 | 更新中途断电/强杀 | 组件半替换 | 以组件为事务单位 + 备份 + 启动时的完整性自检（发现残缺文件提示回滚） |
| R7 | Qt DLL 未随包分发 | 换机打不开 | `deploy.bat` 用 windeployqt 自动补齐，并把 DLL 清单纳入构建产物校验 |
| R8 | 炫彩 VC 无 vcvarsall，环境靠手工拼 | 换机构建失败 | `scripts/env_msvc.bat` 固化 INCLUDE/LIB/PATH，并在 CMake 里做存在性断言，缺失时给出明确错误 |
| R9 | LGPL 合规 | 法律风险 | 随包附 Qt LGPL 说明 + 提供可重链的目标文件获取方式说明 |

---

## 10. 附：ADR 摘要

- **ADR-001 GUI 选 Qt 6 Widgets** — 状态：Accepted。表格/表单/树控件齐全，模型配置管理场景最省力；代价是需分发 Qt DLL。
- **ADR-002 配置真相源为自有 JSON + 单向渲染** — 状态：Accepted。换取备份/回滚/校验能力与内核解耦；代价是 Web UI 内改配置会被覆盖（用 mtime 检测缓解）。
- **ADR-003 不引入 SQLite 与 yaml-cpp** — 状态：Accepted。数据量级与便携性不匹配，JSON 更契合；预留 `IStore` 接口以便将来切换。
- **ADR-004 更新采用混合策略** — 状态：Accepted。优先 Release 包以获得 sha256 校验与原子回滚，保留 git/npm 作为可用性兜底。
- **ADR-005 API Key 默认明文** — 状态：Accepted。便携性优先，主密码为可选增强；DPAPI 因绑定账户会破坏换机能力而否决。
