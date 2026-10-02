# ADR-008：DeepSeek Harness (dsh) 便携包支持

## Status
Accepted (2026-09-23)

## Context

启动器定位从"Hermes 便携包管理器"扩展为"多 Agent 便携平台"（便携包根下每 Agent
一个子目录，`.\Hermes` 之外新增 `.\DSH`）。DeepSeek Harness 是 DeepSeek 官方开源
agent harness（MIT，TypeScript/Cordis，npm 包 `@deepseek-ai/dsh`）。

可行性已实测（详见知识库 PortableAgent/deepseek-harness/，探针在 `_tmp/dsh_eval/`）：

- **纯 Node 运行时**（node:sqlite 需 Node ≥22.5；koffi 3.3.1 原生模块带 win-x64
  预编译；Node v24.21.0 LTS 全链路实测通过），无 Python/uv/git。
- **官方 `DSH_HOME` 环境变量**决定数据根（显式配置 > `$DSH_HOME` > `~/.dsh`），
  无需 Hermes 式 HOME junction；`profiles/node_modules` 的 Windows 链接是 junction
  且**每次启动自动重指向**（dsh-app-boot `healProfilesModuleFallback`），天然可移动。
- **首启零网络**：内置 profile bundle 从安装目录解析，`profiles/web` 自动初始化。
- **会话 token 只在 stdout**：`dsh web: http://127.0.0.1:3080/?token=...`（每 boot
  随机，`.credentials.yaml` 里是另一把持久 grant，不能替代）。默认 401 fenced。
- web profile flags：`--host` `--port`（0=OS 分配）`--no-open` `--trusted-host`。
- `dsh plugin add` 走 pnpm（可选功能，v1 不预装）。

已交付**独立构建脚本** `scripts/build_dsh_portable.py`（Python 参照实现 +
`I:\PortableAgent\DSH` 现役包），启动器内移植为 PortableBuilder 同构的 C++ 流程。

## Decision

### 1. 包布局（`<portableRoot>/DSH/`，构建产物）

```
DSH/
├── Dsh.bat                  ASCII+CRLF 启动脚本（env 钉死在包内，模板在 DshBuilder）
├── node/                    Node.js win-x64（nodejs.org zip + SHASUMS256 校验）
├── dsh/                     npm prefix: node_modules/@deepseek-ai/dsh（锁精确版本）
├── data/dsh/                DSH_HOME（profiles/ sessions .env）
├── _home/…                  HOME/USERPROFILE/APPDATA/LOCALAPPDATA/TEMP 落点（先建后指）
├── studio/                  cache/downloads + npm-cache + pnpm
└── runtime-manifest.json    schema 1：node 版本+sha256 / dsh 版本 / 构建时间
```

### 2. 核心层新增（不碰现有 Hermes 组件）

- **`DshRuntime`**（`hs::dsh`）：路径解析（root=`portableRoot()/DSH`）、钉死环境表、
  `start(port)`（隐藏进程 + **匿名管道后台线程捕获 stdout 首行 token**）、`stop()`
  （记录 pid → 端口反查 → killTree）、`queryVersions()`。
- **`DshBuilder`**（`hs::dsh`）：`resolveLatest()`（npm registry packument 取
  dist-tags.latest + versions；nodejs.org/dist/index.json 取最新 LTS）+
  `run()`（下载/校验/解包 node → `npm install --prefix` 装 dsh → 写 Dsh.bat →
  建目录 → `--dump-default-config` 预初始化 profile → manifest → 逐级校验）。
  复用 HttpClient / Archive(miniz zip) / Hash / proc 原语，与 Python 脚本步骤一一对应。
- **`Types.h`**：`AppSettings` 增 `DshSettings dsh`（webUiPort=3080 / launchOnStart /
  pinnedVersion；`_WITH_DEFAULT` 宏，旧 settings.json 缺字段取默认）。

### 3. 壳层（v2 迭代, commit 324a9b9）

- **顶部三页签** `[启动器 | Hermes | DSH]`：每个 agent 页签带独立相位圆点
  （运行绿/启动青/掉线橙/停止灰），右侧双状态徽标；DSH 页签内嵌 **WebView #3**
  （导航 URL 带会话 token，dsh 重启后按 URL 变化自动重导航）。点击 agent 页签
  未运行时自动拉起并自动切换。
- **可拆分独立窗口**：页签条「⧉ 独立窗口」按钮把当前 agent 视图重挂到无页签条的
  顶层窗口（WebView2 `put_ParentWindow` 官方重挂），两个及以上 Agent 窗口可并排
  同时显示；独立窗 WM_SIZE 同步 Bounds，**关闭独立窗 = 合并回主窗页签**
  （先重挂后销毁，顺序颠倒会连带销毁 WebView2 子 HWND）。WebViews 以
  external 标志保护已拆出视图不被主窗 Hide/Resize 触碰。拆出后主窗对应页签
  显示原生提示页（聚焦/合并按钮）。
- **概览统一卡片**：原三张不一致卡片合并为一张两行式卡片，Hermes/DSH 行完全同构
  （badge + 详情 + PID/内存/时长/端口 指标 + 启动/停止/打开浏览器），环境信息降为
  卡片脚注。
- **更新中心适配**：组件表追加 `dsh`（npm registry dist-tags.latest）与
  `dshnode`（nodejs.org 最新 LTS）两行；`updateCore` = 清 node_modules 重装 +
  UpdateManager 备份回滚；`updateNode` = zip 替换 + 备份回滚；apply 前自动停
  DSH；扫描对 PendingBuild 放宽（DSH 行不依赖 Hermes 核心）。
- **体检**：追加 dsh_install / dsh_versions / dsh_port 三项（WARN 级不阻断 Hermes）。
- **设置**：新增 DSH 卡片（端口 + launchOnStart）；`launchOnStart` 与
  `stopHermesOnExit` 均覆盖 DSH。
- Bridge 命令：`dsh.start / dsh.stop / dsh.openBrowser`、`build.dsh.resolve /
  start / cancel`；`proc.state` 事件含 `dsh` 子对象（phase/pid/mem/uptime/
  tokenReady/detached/installed）。

### 3.1 v1 简版形态（已被 v2 取代）

v1 无 DSH 页签、概览为两张独立卡片、DSH Web UI 仅系统浏览器打开——见 git 历史
e93b4bc。

## Consequences

- Hermes 主路径**零改动**（Types.h 设置宏追加一个成员，向后兼容）。
- dsh 处于 0.1.x RC，官方明示将有破坏性变更——版本发现显示 latest 但构建默认锁
  `pinnedVersion`（当前 0.1.5-rc.2），升级需显式操作。
- npm install 阶段无进度百分比（子进程阻塞），进度事件按阶段跳变；取消只在阶段边界生效。
- DSH 组件更新（dsh 升级 = npm install 新版 + dsh/ 目录备份回滚）留待后续 ADR。

## 实测记录（本 ADR 决策依据）

- Node v24.21.0 LTS：node:sqlite OK / koffi 3.3.1 加载 OK / npm install 522 包 OK /
  `--dump-default-config` OK / web boot 监听 3080 OK。
- `I:\PortableAgent\DSH` 现役包由 `scripts/build_dsh_portable.py` 构建，
  Dsh.bat 全链路 E2E：无 token 401、带 token 303、运行期写入全部落包内。
