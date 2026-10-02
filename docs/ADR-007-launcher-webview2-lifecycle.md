# ADR-007：启动器 v3 演进为 WebView2 嵌入式生命周期启动器

## Status
Accepted（2026-09-21）

## Context

三个历史版本：Qt6（废弃，26MB DLL）、FLTK（保留，全功能）、v3 原生 Win32（现役）。
用户明确定义启动器定位：

1. **最重要的功能是从 0 构建一套完整便携版 Hermes + 组件更新**（ADR-004 的构建能力是产品核心）
2. **启动器 UI 采用 WebView2 内嵌启动器页**（shell.local HTML）
3. **Hermes 启动后自动切到 Hermes 页，启动器退化为悬浮图标**：点击弹菜单（停止等 + 插件注册项），双击返回启动器页

本机事实（2026-09-21 两轮诊断，最终结论；探针证据 `_tmp/wv2probe/` + `D:\GitHub\text\wv2test\WebView2测试报告.md`）：
**WebView2 在本机完全正常**（Runtime 153.0.4234.48，环境/控制器/导航/JS/渲染像素全部实测通过）。
此前"执行即退 exit 13 / 渲染层损坏"的结论是**探针自身的 bug**，共三处，均已修复并验证：

1. **独立探针缺 controller AddRef**：CreateCoreWebView2Controller 回调给出的控制器若不
   `AddRef` 保存，回调返回即被销毁 → 导航静默死亡（TIMEOUT 假象）。修复：保存引用。
2. **启动器 Wv2_Resize 漏存 lastRc**：只下发 Bounds 未记录矩形 → 像素终审拿 (0,0,0,0)
   采样 0 像素 → 恒判"渲染空白"回退。修复：`g.lastRc = rc`。
3. **ExecuteScript 上下文敏感性（本机特性）**：从 NavigationCompleted / WebMessageReceived
   等回调上下文调用立即生效；从 WM_TIMER / 后台 marshal 上下文直调**静默失败**。
   修复：宿主→页面事件统一入队 + 30ms 节流 SetTimer 冲刷（timer 上下文实测可靠），
   同时消除启动竞态（页面加载快于探测时响应丢失）。

## Decision

### 1. 交互模型（生命周期）
```
启动 → WebView2 #1 启动器页（HTML 全功能）
     → [空目录/半成品] 「待构建」模式首屏=构建向导；[完整] 常规页
     → 页内启动 Hermes → 端口就绪 → 自动切 WebView2 #2 Hermes 页 + FabBtn 淡入
     → 点 FabBtn 弹菜单（停止/重启/日志/状态/插件动作/返回启动器）；双击 = 回启动器页
     → 停止 → 自动回启动器页，FabBtn 淡出
托盘 + Ctrl+Shift+L 兜底
```

### 2. HTML 嵌入引擎选型（回应"除了 WebView2 还有什么"）

| 方案 | 体积 | 结论 |
|---|---|---|
| WebView2 重装修复 | 0 | 已试，本机无效果；健康机器首选 |
| CEF | +150~200MB 随包 | 备选：自带 Chromium 必定可跑，体积冲突便携哲学 |
| Qt6 WebEngine | +~100MB | 备选：精简 Qt 未含模块 |
| Sciter | +6MB 单 DLL | 备选：体积最优，但 HTML 方言改写 + 商用授权 |
| MSHTML(IE) 控件 | 0 | 不推荐：IE11 引擎无现代 JS/CSS |
| 原生 GDI | 0 | **已选为本机回退**（核心功能保留档） |

路线：WebView2-first 代码 + 原生回退（概览/构建★/更新★/插件/设置）；运行时三级探测
（Controller 创建 → ExecuteScript 结果校验 → 屏幕像素终审）失败即回退，Runtime 修复后自动回到完整体。
若未来彻底放弃 WebView2，优先评估 Sciter（体积），其次 CEF（确定性）。

### 3. 架构
```
HermesStudio.exe（Win32 壳 ~1.2MB + WebView2LoaderStatic）
  ├─ WebView2 #1 shell.local/index.html（vanilla JS 暗色，全功能 UI）
  │    ↕ WebMessage JSON 桥（唯一通道；页面→命令，宿主→事件）
  ├─ WebView2 #2 http://127.0.0.1:<port>（绝不挂宿主接口/桥，安全边界同方案2 第七节）
  ├─ FabBtn/FabMenu（Hermes 页激活时；菜单项含插件动作+confirm）
  ├─ 托盘/热键；原生 GDI 回退面板
  └─ 核心层 0 改动接入：src/core|app|platform|storage 16 cpp + miniz(C)
       链接 +bcrypt +winhttp +ws2_32 +version
```

### 4. WebMessage 桥协议（v1）
页面→宿主：`window.chrome.webview.postMessage({...})`；宿主→页面：`window.__hostEvent(evt)`（ExecuteScript 注入）+ 周期 `proc.phase` 推送。

| 命令 (cmd) | 参数 → 响应/事件 |
|---|---|
| proc.start / proc.stop / proc.restart | veto 检查 → `{ok,message}`；phase 事件流 |
| proc.openBrowser / proc.getState | — / `{phase,pid,memMB,uptimeSec,startingSec,port}` |
| build.resolve | → `build.remote` 事件 `{runtime:{version,size,sha256},webUi:{...}}` |
| build.start | `{targetDir,useCache,mirror}` → `build.progress{percent,stage}` 流 → `build.done{ok,cancelled,message,runtimeVersion,webUiVersion}` |
| build.cancel | 置取消标志 |
| update.scanLocal / update.scanRemote{channel} | → `update.list{components:[...]}` |
| update.apply{componentId} | 停机 → checkRemote → apply(progress 流) → 自动重扫 |
| update.nodeVersions / update.applyNode{version} | → `update.nodeList{versions}` |
| update.backups / update.rollback{component,path} | → `update.backupList` / `{ok,message}` |
| preflight.run / preflight.fixAll / preflight.fix{id} | → `preflight.result{items,summary}` |
| provider.list/add/update/remove/toggle；model.* 同构 | models.json CRUD → `{ok,error?}` |
| plugin.list / plugin.toggle{id} / plugin.remove{id} / plugin.action{plugin,action} | uiEntries 区块 + confirm 由页面渲染 |
| settings.get / settings.save{appSettings} | studio/settings.json |
| log.query{level,keyword,limit} / log.export{path} | launcher.log |

安全：桥只挂在 #1；命令白名单分发；插件 action 走 PluginHost::executeAction；文件操作限包内。

### 5. 关键行为决策
- **HermesProc 减薄**：Start/Stop 委托 `hs::Launcher`（获得 veto/StartResult），Phase ↔ HermesState 映射；端口/PID/内存查询保留 GetExtendedTcpTable（1s 轮询）
- **零依赖启动**：exe 放空目录 → 「待构建」模式（Paths 延迟绑定；构建成功后 setRoot 热初始化）； FabBtn/托盘/设置在此模式照常
- **通道真实生效**（修 Qt6 恒 stable 缺陷）；**autoCheckUpdate 消费**（启动 0.8s 自动远端检查，FLTK 行为）
- **插件声明式 UI**：uiEntries → HTML 动态区块 + FabMenu 项同源；启停/删除后重建（优于 Qt6 只建一次）
- **日志落盘** `studio/logs/launcher.log`；HTML 日志页过滤/搜索/导出
- **原生回退档**（用户选定"核心功能保留"）：概览（生命周期 CTA）/构建简版（resolve+start+cancel+progress+日志）/更新简版（5 组件表+通道+更新选中+备份回滚）/插件/设置/关于；Provider/模型/体检/日志 → "需 WebView2"引导卡

## 补记（2026-09-21）：单文件发布形态

应用户要求，发布形态定为**单文件 exe、体积小、只在运行目录释放数据**：

- 启动器页编译进 exe（app.rc RCDATA：`launcher_v3/shell/{index.html,logo.png}`，logo 压至 128px/22KB）
- 运行时经 `WebResourceRequested` 拦截 `http://app.local/*` 从内存应答，**零文件释放**（不再依赖外部 shell.local/）
- **API 要点**：`AddWebResourceRequestedFilter(uri, COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL)` 与 `add_WebResourceRequested(handler)` 缺一不可，且必须在 Navigate 前注册；只挂 handler 事件永不触发（本机实测踩坑）
- exe 1.16MB；只写运行目录审计表（WebViewData/studio/data 均在包内，%LOCALAPPDATA% 等零写入）
- shell.local/ 源文件仍在 launcher_v3/shell/（改 UI 需重编 exe，这是单文件形态的固有代价）

## Consequences
- 启动器首次具备完整生命周期：空目录 → 构建 → 运行 → 更新 → 回滚
- 本机（WebView2 视觉损坏）即回退形态，核心功能不受阻；Runtime 修复零改动回到完整体
- HTML UI 与 C++ 解耦：后续页面迭代不碰壳层；桥协议是唯一契约
- 代价：HTML 页与原生回退双实现（回退仅核心档，可控）；桥协议需版本化管理（settings.schemaVersion 同法）
