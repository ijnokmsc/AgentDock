# AgentDock

> Windows 桌面 **Agent 启动器 / 便携包宿主**（原名 HermesStudio）。
> 一个窗口完成「体检 → 配模型 → 更新 → 启动」，把 Agent 内核当黑盒：内核零改动，所有改造都在外层。

## 这是什么

AgentDock 是 Hermes / DSH 等 Agent 便携包的宿主外壳：

- **配置真相源**：启动器自有 JSON（`models.json` / `settings.json`），启动前渲染导出 `.env` + `config.yaml`，再拉起内核进程（CLI / WebUI / Desktop）。
- **更新中心**：组件注册表驱动，内核 / web-ui / Python / Node 运行时 / 启动器自身独立更新，Release 包 sha256 校验 + 备份 + 回滚，失败可回退 git / npm 增量对齐。
- **体检（Preflight）**：MSVC / SDK / 运行时 / 连通性 / 端口 / 代理一键检查。
- **插件系统（HSPP）**：原生 C ABI DLL 插件，挂启动器导航与悬浮菜单，见下文。
- **便携化**：PortableBuilder 把运行产物打成「exe + data + 内核 + 运行时」同级的绿色目录，随处可搬。

## 仓库结构

| 目录 | 内容 |
|---|---|
| `launcher_v3/` | **现役启动器 AgentDock**：双模（WebView2 shell + 原生 Win32 回退）、悬浮球/悬浮菜单（多 Agent 均等）、插件快捷区、页签可拆分独立窗口 |
| `src/core/` | 宿主核心，纯 C++17 无 Qt 依赖：PreflightChecker · ModelRegistry · ConfigRenderer · Updater · PluginHost · PortableBuilder · AgentSpec · ProviderPresets |
| `src/platform/` | Win32 实现：WinHTTP（含代理）· BCrypt（sha256/AES-GCM）· miniz · 进程/端口/注册表 |
| `src/ui/` | Studio 主程序（Qt Widgets）页面层 |
| `include/hs_plugin.h` | HSPP 插件 ABI 头（`hs_*` C ABI） |
| `examples/` | 插件示例与自测（clear-logs 体积优化插件、demo_plugin 等） |
| `docs/` | 架构 / 插件协议 / ADR-003~009 / 便携构建指南 / 功能清单 |
| `scripts/` | 构建 / 部署 / Qt 引导 / MSVC 环境注入 |
| `tests/` | smoke · e2e · launch · remote · resolve |

## 插件协议 HSPP v1.4

插件 = `<便携包根>/plugin/<plugin-id>/` 下的 `plugin.dll`（导出 `hs_plugin_entry()`）+ `plugin.json` 清单。协议要点：

- 清单声明 `actions`，支持 `type` / `default` / `quick` / `arg`（`"checklist"` 时 UI 回传勾选项）；
- 动作执行期间插件可用 `@@DATA@@{json}` 日志行回传**结构化结果**，随 `plugin.action.result` 事件直达 UI；
- 支持 zip 一键安装；ABI 版本向上兼容，老插件零改动可用。

完整规范见 [docs/PLUGIN-PROTOCOL.md](docs/PLUGIN-PROTOCOL.md)，宿主实现见 `src/core/PluginHost.cpp`。

## 构建

工具链为无 `vcvarsall` 的裁剪版 MSVC，环境注入与工具见：

- `scripts/env_msvc.bat` — 注入 INCLUDE/LIB（MSVC + Windows SDK）
- `scripts/bootstrap_qt.py` — aqtinstall 拉取 Qt 6.8.3 最小子集到 `thirdparty/`（**SDK 不入库**）
- `scripts/build.bat` — Studio 宿主（CMake + Ninja）
- `launcher_v3/build.bat` — 现役启动器
- 便携打包见 [docs/PORTABLE-BUILD-GUIDE.md](docs/PORTABLE-BUILD-GUIDE.md)

## 相关文档

| 文档 | 说明 |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | 总体架构：分层、依赖倒置、数据流、决策基线 |
| [docs/PLUGIN-PROTOCOL.md](docs/PLUGIN-PROTOCOL.md) | HSPP v1.4 插件协议规范 |
| [docs/ADR-003~009](docs/) | 更新中心 / 便携打包 / GUI 选型(SoUI4·FLTK→Webview2) / DSH 便携化 / Agent 注册表 |
| [docs/启动器功能清单.md](docs/启动器功能清单.md) | 启动器功能全集 |

## 版本

- **v0.1.0**（即将发布）：launcher_v3 双模启动器、HSPP v1.4、更新中心、DSH 便携化、体积优化插件。

---

© 2026 ijnokmsc — 保留所有权利 (all rights reserved)。
