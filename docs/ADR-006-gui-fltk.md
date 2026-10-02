# ADR-006：启动器 GUI 改用 FLTK（替代 SOUI4）

## Status
Accepted

## Context

启动器需要减少运行时 DLL（原 Qt 6 方案拖 26MB/30 个文件）。先后评估了两个候选：

### 方案对比（同一控件集合的 demo 实测）

| 维度 | FLTK 1.4 | SOUI4 |
|---|---|---|
| demo exe 体积 | **~0.5MB** | 5.8MB |
| 依赖 | 系统 DLL + VC 运行库 | 系统 DLL（含 skia） |
| 许可 | **LGPL+例外（免费可静态）** | 商用收费 |
| UI 编程 | 命令式原生控件 | XML 声明式 |
| 标题栏 | 系统原生 | 自绘 |
| 可视化设计器 | fluid.exe | uieditor |
| 开发难度 | **低**（API 直观，一次编译通过） | 中（XML+自绘+工程接入坑多） |
| 现代扁平样式 | 原生控件观感（偏系统经典） | 自绘可深度定制（视觉上限高） |

### 用户决策
用户先给 SOUI4 UI 样式打 6 分，后明确决定：**采用 FLTK 开发启动器**。

## Decision

**用 FLTK 1.4 重写启动器 UI 层，复用核心层（5530 行纯 C++，零 Qt 依赖）。**

### 架构
```
HermesStudio.exe  (FLTK, 约 0.7MB, 单文件零第三方 DLL)
  ├── 核心层 (0 改动复用): src/core|app|platform|storage (16 .cpp)
  │      Paths/Launcher/PortableBuilder/Updater/HttpClient/Archive/JsonStore...
  ├── UI 层 (重写为 FLTK): launcher_fltk/main.cpp
  │      左侧导航 (Fl_Button 单选) + 右侧页面 (Fl_Group hide/show 切换) + 底部状态栏
  └── 第三方: FLTK 静态库 + miniz (核心层需要) + nlohmann/json (头文件)
```

### 已验证
- FLTK 1.4.5 静态编译成功（36MB 源码，产 fltk*.lib + fluid.exe 设计器）。
- FLTK 启动器 677KB 单文件，核心层 16 个 .cpp 全部编入。
- 部署到便携包根目录运行成功（autoDetect + 窗口 + Responding）。
- dumpbin 确认依赖 = 系统 DLL + VC 运行库，**无 Qt/SOUI/FLTK 第三方 DLL**。
- Qt 版 5 个 Qt6 DLL（26MB）现在可删（FLTK 不需要）。

## Consequences

**更容易**：
- 体积从 Qt（2.2MB exe + 26MB DLL）降到 **0.7MB 单文件**
- 免费许可（LGPL+例外），无 SOUI4 的商用授权成本
- 开发简单（命令式 API，一次编译通过）
- 核心层完全复用，零改动

**更困难**：
- FLTK 是命令式坐标布局（无 layout manager），复杂页面需手动算坐标或用 fluid
- 原生控件观感偏系统经典风格，现代扁平样式需自绘（FLTK 有 Fl_Box 自绘能力但无 SVG 皮肤体系）
- UI 层 2716 行 Qt 需重写
- 无 Qt 的信号槽/并发（QtConcurrent 后台线程需换成 FLTK 的 Fl::awake 或 std::thread）

## 待办（按序）
1. [x] FLTK 静态库编译 + demo 验证
2. [x] FLTK 启动器骨架（导航 + 页面切换 + 概览页）
3. [x] 核心层接入 + 编译 + 部署 + 运行验证
4. [ ] 逐页迁移（体检/Provider/模型/更新/构建/插件/设置/日志）
5. [ ] 后台任务线程化（FLTK 单线程消息循环 + std::thread + Fl::awake）
6. [ ] 清理便携包内 Qt6 DLL（26MB）
