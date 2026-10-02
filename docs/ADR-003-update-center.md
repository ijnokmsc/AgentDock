# ADR-003: 更新中心 (M3) 架构设计

## Status
Proposed (2026-09-19)

## Context

启动器要替代配置中心后，还需负责 Hermes 全家桶的更新。当前各组件现状：

| 组件 | 当前版本 | 安装形态 | 更新源 |
|------|---------|---------|--------|
| Hermes 内核 | 0.20.5 | `pip install -e` editable，源码树在 `hermes-agent/`，非 git 仓库 | GitHub `NousResearch/hermes-agent` Release（源码 tar.gz/zip） |
| Web UI | ekko-studio 0.7.23 | npm 包，`node/node_modules/hermes-web-ui` | npm registry `ekko-studio` |
| Node 运行时 | 22.15.0 | `node/node.exe` + 依赖 | nodejs.org 官方 dist |
| uv | 0.12.5 | `uv.exe` | GitHub Release |
| Python 运行时 | 3.12.14 | `python/cpython-*` | python-build-standalone Release |

基础设施缺口：
- **无 HTTP 客户端**（无 WinHTTP/curl/miniz 封装）
- **无解压能力**（Release 包是 tar.gz/zip）

## Decision

### 1. 网络层：WinHTTP（系统自带，零第三方依赖）
- 用 `WinHTTP` API 实现 `HttpClient`：GET / 下载到文件 / 支持代理（System/Direct/Manual）/ 超时 / 进度回调。
- 理由：便携零依赖、支持系统代理、exe 体积不变。libcurl 需引入第三方 DLL，违背便携定位。
- 代理三态：`System`（读注册表/IE 设置）→ `Direct`（直连）→ `Manual`（host:port）。默认 System，可覆盖。

### 2. 解压：miniz（zip）+ 内置 tar 解压
- `miniz` 单头文件（第三方，拷贝进 thirdparty）解 zip。
- tar.gz：先 miniz inflate 解 gzip，再内置解析 tar（tar 格式简单，不需第三方库）。
- 理由：GitHub Release 源码包多为 tar.gz；zip 也需支持。

### 3. 更新策略：混合（优先 Release 包，回退 git/npm）
- **内核**：优先下 GitHub Release 源码包（tar.gz，sha256 校验）→ 解压替换 `hermes-agent/` → 重装 pip editable（`python -m pip install -e hermes-agent`）。无 Release 时回退。
- **Web UI**：`npm install ekko-studio@<ver>` 更新 `node_modules/hermes-web-ui`。
- **运行时**（node/uv/python）：从各官方源下 Release 二进制，校验后原子替换（只替换 exe + 必要文件，node 绝不动 `node_modules`）。

### 4. 原子替换 + 备份 + 回滚
- 更新前把当前组件目录完整备份到 `studio/backups/<component>/<ver>/`（保留 N 份，默认 3）。
- 应用：下载到 `studio/cache/downloads/` → sha256 校验 → 解压到临时目录 → **先备份旧版 → 原子替换**（同名目录用 rename，跨盘复制）→ 删除临时目录。
- 回滚：从备份目录把旧版恢复到原位。
- 失败中断时自动回滚到备份。

### 5. 版本比对
- `compareVersion`（已有）支持 SemVer、日期版、带 v 前缀。channel 支持 stable/beta。

### 6. 风险分级
- Kernel/WebUI 更新：中等风险（需重装 pip editable）。
- Node/uv：低风险（二进制替换）。
- **Python 运行时：高风险**（涉及 venv 有效性），放最后 + 强制二次确认。

## Consequences

**变容易**：
- 一个按钮完成全家桶版本检查与更新
- 每次更新有备份可回滚，不怕更新坏了

**变难**：
- 每个组件的更新流程不同，需要 ComponentAdapter 抽象
- 内核 pip editable 重装对 hermetic 环境敏感，失败需回滚
- 网络层需处理代理切换、断点、证书

## 实施顺序
1. HttpClient（WinHTTP）→ 2. 解压（miniz+tar）→ 3. checkRemote（各源适配）→ 4. 下载/校验/备份/应用/回滚 → 5. UI 接线
