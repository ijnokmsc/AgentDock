# ADR-004：启动器一键构建便携版

## Status
Proposed

## Context

用户需求：**不依赖任何第三方便携版**，只用启动器（HermesStudio.exe）就能搭建一个完整、
可运行、可移动的 Hermes 便携版。

已在 `D:\text\HermesPortable` 手工验证了构建流程可行：
- 官方预编译运行时 `hermes-runtime-hermes-agent-0.21.3-win-x64.tar.gz`（543MB）
- web-ui 包 `hermes-web-ui-0.7.23.tar.gz`（126MB，npm 包 `ekko-studio`）
- 组装后端到端启动成功（web-ui :8648 + agent-bridge :18765）

启动器已具备全部底层能力，**无需新引入第三方库**：
- `HttpClient::downloadFile` —— 下载
- `Archive` —— tar.gz / zip 解压
- `Hash` —— sha256 校验
- `Updater` —— 更新内核 / web-ui（含 apply/backup/rollback）
- `Launcher` —— 启动 / 停止
- `RuntimeManager` —— 已识别 runtime 结构

## Decision

新增一个 **"一键构建便携版"** 功能（GUI 页面 + 后台任务），流程如下：

### 构建流程（复用现有能力）

```
┌─ 用户点击「构建便携版」
│
├─ 1. 选择目标目录 (默认便携包根/子目录)
│
├─ 2. 后台任务 PortableBuilder::run():
│     a. 下载 runtime tar.gz   (HttpClient, 进度条)
│     b. 校验 sha256           (Hash, 对照官方 manifest)
│     c. 解包 runtime          (Archive → python/ node/ git/)
│     d. 下载 web-ui tar.gz    (HttpClient)
│     e. 解包 web-ui           (Archive → webui/)
│     f. 组装目录: data/ _home/ + _home\.hermes junction
│     g. 写 Hermes.bat         (纯 ASCII 启动脚本)
│     h. 修正 pyvenv.cfg home  (绝对路径, 可重定位)
│     i. 校验: import hermes_cli + hermes.cmd --version
│
├─ 3. 完成 → 提示「便携版已就绪, 是否启动?」
└─ 4. 用户可「启动」→ Launcher::start(WebUI)
```

### 新增模块 `PortableBuilder`

```
class PortableBuilder {
    struct BuildOptions {
        fs::path targetDir;        // 便携版目标目录
        std::string runtimeUrl;    // runtime tar.gz URL
        std::string runtimeSha256; // 校验和
        std::string webUiUrl;      // web-ui tar.gz URL
        std::string webUiVersion;  // 版本
    };
    struct BuildResult { bool ok; std::string message; };

    BuildResult run(const BuildOptions& opts,
                    std::function<void(int,const std::string&)> progress);
    // 内部复用: HttpClient / Archive / Hash / Launcher
};
```

### 资源来源（官方 releases）
- **runtime**: `https://github.com/EKKOLearnAI/hermes-studio/releases/download/hermes-0.21.3-runtime/hermes-runtime-hermes-agent-0.21.3-win-x64.tar.gz`
- **manifest**: `.../hermes-runtime-win-x64.json`（含 sha256 + 版本信息）
- **web-ui**: npm 包 `ekko-studio` / `hermes-web-ui`（或 GitHub release 的 tar.gz）

> 启动器运行时从官方 `hermes-runtime-<plat>.json` 拉取最新版本清单，自动匹配平台，
> 无需硬编码版本号。

### 关键实现要点

1. **平台适配**：`win-x64` / `mac-x64` / `linux-x64` 等，从 RuntimeManager 的平台探测复用
2. **pyvenv.cfg 可重定位**：Hermes.bat 每次启动把 `home` 重写为当前绝对路径
3. **HOME 劫持**：`Hermes.bat` 设 `HOME/USERPROFILE=%HERE%\_home`，数据全落便携包内
   - ⚠️ **只劫持 `HOME`/`USERPROFILE` 不够**。实测 web-ui 启动会跑 `npm prefix --global`，
     npm 在 Windows 上硬编码用 `%LOCALAPPDATA%\npm-cache` 与 `%APPDATA%\npm`，每次启动
     在宿主留下 6 个日志；Node 编译缓存写 `%TEMP%`。必须一并钉住
     `APPDATA` / `LOCALAPPDATA` / `TEMP` / `TMP` / `NPM_CONFIG_CACHE` / `NPM_CONFIG_LOGS_DIR` /
     `NPM_CONFIG_PREFIX` / `UV_CACHE_DIR` / `PIP_CACHE_DIR` / `XDG_*` / `PLAYWRIGHT_BROWSERS_PATH`。
     **且必须先把 `_home\...` 目录建出来再指过去**（`TEMP` 不存在会让 runtime 直接失败）。
     完整清单与实测数据见 `docs/PORTABLE-LEAK-AUDIT.md`。
4. **editable 可移动**：官方 runtime 的 `.pth` 是相对 `../../..`，天然可重定位；构建后验证 `import hermes_cli`
5. **进度与取消**：下载/解包走 `progress` 回调，支持取消

## Consequences

### 变得更简单
- **摆脱第三方**：启动器自带构建能力，无需下载别人打包好的便携版
- **可复现**：每次构建都用官方 releases 的固定版本 + sha256 校验
- **可升级**：构建后仍可用现有 Updater 更新内核/web-ui

### 变得更复杂
- 首次构建需下载 ~670MB（runtime + web-ui），依赖网络
- 构建流程较长（数分钟），需要良好的进度/错误提示
- 需处理平台差异（Windows junction vs macOS/Linux symlink）

### 权衡
- **官方 runtime 优先**：直接下预编译 runtime（快、官方验证可重定位）
- **备选：源码构建**：若想要最新 main，走 `tools/build.py`（第 4 章），但慢
- 两条路径都保留，构建页提供「预编译（推荐）」/「源码」两种模式

## 后续
- 实现 `PortableBuilder` 模块 + 构建页 UI
- 集成下载缓存（同版本不重复下载）
- 校验失败自动回滚（删除半成品目录）
