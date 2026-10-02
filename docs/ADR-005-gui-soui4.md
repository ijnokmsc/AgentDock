# ADR-005：启动器 GUI 从 Qt 迁移到 SOUI4（单文件零 DLL）

## Status
Proposed

## Context

HermesStudio 启动器当前用 **Qt 6 (Widgets)** 构建 GUI，随包携带大量运行库：

| 项 | 数量/体积 |
|---|---|
| Qt 主 DLL | Qt6Core/Gui/Network/Svg/Widgets，**26MB** |
| Qt 插件目录 | platforms/styles/iconengines/imageformats/networkinformation/generic/tls，**10 个 DLL** |
| 总计 | **30 个文件 / 26MB** |
| HermesStudio.exe 本体 | **2.2MB** |

用户诉求：**DLL 太多，改用原生 GUI 或支持「资源可内嵌 PE」的库**，实现绿色单文件发布。

**核心洞察（决定方案的关键事实）**：
- 启动器的**业务/核心层**（`src/core/` `src/app/` `src/platform/` `src/storage/`，**5530 行**）
  是纯 C++，**零 Qt 依赖**（Paths/Launcher/PortableBuilder/Updater/HttpClient/Archive/JsonStore…）。
  迁移**不需要动这一层**。
- 只有 **UI 层**（`src/ui/` + `src/main.cpp`，**2716 行**，10 个页面）依赖 Qt，需重写。

## Decision

**选型：SOUI4（`gitee.com/setoutsoft/soui4`）+ Direct2D 渲染 + PE 资源内嵌 + 全静态链接（LIB_CORE + LIB_SOUI_COM + 静态 CRT）。**

### 选型过程（含一次重要纠偏）

用户最初选择 **SOUI3 + Direct2D**。调研发现 **SOUI3 只有 GDI/Skia，没有 Direct2D**——
`render-d2d` 组件是 **SOUI4** 才加入的。经确认，改用 **SOUI4**（与 SOUI3 同为商用授权，
但支持 D2D、控件更全、2026-09 仍在活跃更新）。

候选方案 trade-off：

| 方案 | 文件数/体积 | 重写成本 | 许可 | 结论 |
|---|---|---|---|---|
| **Qt 静态链接** | 1 exe ~25-30MB | 最低（改 CMake） | LGPL 合规成本高 | 体积仍大，放弃 |
| **Win32 原生** | 1 exe ~1-2MB | 最高（全手写控件/布局/托盘） | 无 | 成本过高 |
| **FLTK/Dear ImGui** | 1 exe ~几 MB | 高（重写 + 自绘主题） | MIT/宽松 | 可用但生态弱 |
| **SOUI4（选定）** | **1 exe ~3-5MB** | 中高（重写 UI，核心复用） | 商用授权（已确认接受） | **选它** |

### 可行性验证结果（已完成）

用当前工具链（CMake + Ninja + MSVC 14.41）最小化配置 SOUI4：
```
SOUI_ENABLE_CORE_LIB=ON  SOUI_ENABLE_COM_LIB=ON  SOUI_SHARED_CRT=OFF
SOUI_BUILD_DEMOS/GAMES/TOOLS=OFF  SOUI_ENABLE_SVG/SPY/ACC/HTTPCLIENT=OFF
```
**编译通过**：`soui4.lib`（20MB）、`render-d2d.lib`（815KB）、`utilities4.lib`（1MB），
仅 `/MD→/MT` 重写警告，无错误。输出确认：
- `---Building [soui] with LIB_CORE`（核心静态链接）
- `---Building [soui components] with LIB_SOUI_COM`（组件静态链接）
- `Building with MT`（静态 CRT）
- `enable d2d1.1`（Direct2D 已启用）

### 目标架构

```
HermesStudio.exe  (单文件, 内嵌全部 UI 资源 + 静态链接 SOUI4)
  ├── 核心层 (0 改动, 纯 C++): Paths/Launcher/PortableBuilder/Updater/HttpClient/Archive/JsonStore...
  ├── 应用胶水层 (新): 包装核心层为 SOUI 可调用的接口
  └── UI 层 (重写): SOUI XML 布局 + C++ 事件绑定 (10 个页面)
        ├── 导航 + 页面切换 (SHostWnd / 页面容器)
        ├── 概览/体检/Provider/模型/更新/构建/插件/设置/日志
        └── 托盘 (SOUI STrayIcon) / 对话框 (SHostDialog) / 表格 (SListView)
```

### 关键风险与缓解

| 风险 | 缓解 |
|---|---|
| SOUI4 编程模型（XML 声明式 DirectUI）与 Qt（保留模式控件）差异大 | 分页迁移，每页先出垂直切片验证再展开；核心层接口不变 |
| 表格/树/托盘等复杂控件需对应 SOUI 控件（SListView/STreeCtrl/STrayIcon） | 先查 SOUI 控件能力，评估是否有能力缺口 |
| SOUI4 是较老框架，文档多为中文论坛/Q群 | 已拉取完整源码 + demo，可对照；有 `soui-sys-resource-light` 现代亮色主题 |
| 商用授权需联系作者 | 用户已确认接受付费；发布前联系作者登记 |
| 静态链接 exe 体积 | 全静态 + 只链接 render-d2d（不链 Skia），预计 3-5MB |

## Consequences

**更容易**：
- 发布单 exe，零 DLL，便携包根目录从 30 个 Qt 文件降到 1 个
- 体积从 26MB 降到 ~3-5MB
- 资源内嵌 PE，UI 与逻辑彻底分离（XML 改版不改代码）

**更困难**：
- UI 层 2716 行需完整重写（Qt→SOUI），且 SOUI 编程模型不同
- SOUI4 商用需付费授权
- 学习成本（SOUI 生态/文档在国内）
- 核心层虽零改动复用，但 QtConcurrent 后台线程需换成 SOUI/原生线程模型

## 待办（按序）
1. [x] 可行性验证：SOUI4 静态编译 + Direct2D + MSVC 14.41
2. [ ] 搭最小垂直切片：单 exe Hello World + 资源内嵌 PE + 一个页面，确认最终形态
3. [ ] 抽取 UI 层业务逻辑到独立接口层（解耦 Qt）
4. [ ] 逐页迁移（10 页）到 SOUI XML + C++ 事件
5. [ ] 编译验证 + 端到端回归核心层
