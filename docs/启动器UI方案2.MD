**Hermes 便携启动器 —— 悬浮入口 + 浮动菜单设计方案（B+C 架构修订版）**

> 修订点：悬浮按钮由「单击即返回」改为「**单击展开快捷菜单 / 双击直达启动器**」，其余 B+C 架构（双 WebView2 + 托盘 + 全局热键）不变。

---

### 一、设计目标与硬约束

| 目标 | 说明 |
|---|---|
| **零依赖 Hermes 实现** | 悬浮按钮与菜单全部由宿主原生绘制，不注入 DOM、不读 Hermes 样式、不受其 CSP/z-index/版本升级影响 |
| **物理隔离安全边界** | Hermes 页面（`127.0.0.1:17520`）**永不暴露 host object**；悬浮入口是纯 Win32 HWND，攻击面为 0 |
| **单手可达 + 可避让** | 默认右下，支持拖拽换位置、记忆位置、贴边吸附 |
| **单文件、不留痕** | 无注册表、无 %APPDATA%；位置与菜单配置存在移动硬盘的 `config/launcher.json` |
| **双击不误触、单击不迟钝** | 单击/双击消歧必须有确定性的时间窗与视觉反馈 |

---

### 二、架构回顾：三个 HWND 的职责

```
+--------------------------------------------------+
|  MainWnd (原生壳：标题栏 / 导航 / 状态栏)          |
|  +--------------------------+                   |
|  | WebView2 #1 shell.local  |  ← 启动器自己的页   |
|  |   (home / 插件页 / 设置)   |     挂 host object |
|  +--------------------------+                   |
|  +--------------------------+                   |
|  | WebView2 #2 127.0.0.1    |  ← Hermes，无 host object
|  +--------------------------+                   |
|                          [●]  ← FabBtn  (TOPMOST sibling)
+--------------------------------------------------+
   ^ Tray icon + 全局热键 Ctrl+Shift+L  (C 部分，兜底)
```

- **FabBtn**：48×48（按 DPI 缩放）圆角分层窗口，`WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE`。
- **FabMenu**：独立分层弹出窗口，`WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE`，**不属于 Hermes 的 HWND 树**，因此 Hermes 的模态框盖不住它，它也不会抢焦点。
- 两者都设 `NOACTIVATE`：点击它们不会打断 Hermes 里的输入焦点——这是整个体验能否成立的前提。

---

### 三、交互设计：单击 vs 双击的消歧

#### 3.1 为什么不能「单击立刻弹菜单」

若单击立刻弹菜单，双击的第一击就会先弹出菜单再收回，产生明显闪烁，且用户会怀疑「我到底点了没有」。正确做法是**延迟判定**，用系统双击时间窗做决策。

#### 3.2 状态机

```
IDLE ──LButtonDown──► ARMED（启动 T 毫秒定时器，按钮压态）
   ▲                      │
   │  超时且无第二次点击     │  T 毫秒内再次 LButtonDown
   │                      ▼
   └──────────────  SHOW_MENU ◄─── 立即停定时器
                         │
                  选中项 / ESC / 失焦 / 点击外部
                         ▼
                       IDLE
```

- `T = GetDoubleClickTime()`（通常 500ms），**不做固定值硬编码**，随系统设置走，符合用户肌肉记忆。
- **ARMED 阶段必须给视觉反馈**：按钮 scale 0.92 或亮度降低，让用户知道「已按下，松手或再点一次都有事发生」。这样 500ms 等待不会感觉像卡死。
- **双击路径必须快**：第二击落下瞬间立刻 `GoToLauncher()`，不等鼠标抬起，双击手感才脆。
- **防抖**：菜单已显示时，FabBtn 忽略双击判定（避免用户在菜单上连点触发返回）。

#### 3.3 伪代码

```cpp
// WM_LBUTTONDOWN
void FabBtn::OnLButtonDown() {
    if (menuVisible) return;                 // 菜单开着时不进入判定
    if (armed) {                             // 时间窗内第二次点击 → 双击
        KillTimer(hTimer); armed = false;
        SetVisual(IDLE);
        GoToLauncher();                      // 直达，不等抬起
        return;
    }
    armed = true;
    SetVisual(ARMED);                        // 压态，消除迟滞感
    hTimer = SetTimer(hwnd, ID_DCLICK, GetDoubleClickTime(), nullptr);
}

// WM_TIMER(ID_DCLICK)
KillTimer(hTimer); armed = false;
SetVisual(IDLE);
ShowMenuAt(this, AnchorBottomRight);         // 超时 → 弹菜单
```

> 可选增强：在配置里提供 `fab.doubleClickTimeMs` 覆盖值，以及 `fab.singleClickMode: "immediate"` 给嫌延迟的用户（此时双击退化无效）。

---

### 四、浮动菜单设计

#### 4.1 形态

- 宽度 200–240px（DPI 缩放），圆角 8px，阴影 + 半透明背景（Acrylic 可选，但会拖慢首帧，建议纯色 `#1e1e1e` + 95% 不透明）。
- 锚定在 FabBtn **上方 8px 居中对齐**；若超出工作区上沿则改为出现在按钮下方。
- 出现方式：120ms 淡入 + 向上滑 8px；消失：80ms 淡出。**不做长动画**，工具类 UI 动画超过 150ms 就会显得拖沓。
- 消失条件：ESC、失去焦点（`WM_ACTIVATEAPP` / 点击外部）、选中项、启动器切页、Hermes 全屏类操作。
- 键盘：菜单弹出后 ↑↓ 导航、Enter 执行、Esc 关闭；FabBtn 本身给一个 `Alt+`` 快捷键作为纯键盘入口。

#### 4.2 默认菜单项（建议 6 个以内，超过就折叠）

| 项 | 图标 | 动作 | 类型 |
|---|---|---|---|
| 返回启动器 | ⌂ | 切 WebView2 #1 到 home | **主项（加粗/高亮）** |
| 查看日志 | 📄 | 打开日志面板 / 或直接 `ShellExecute` 打开 data/logs | 内置 |
| 清理日志 | 🧹 | 调清理插件（见 4.3） | **插件** |
| 重启网关 | ↻ | 杀进程树 + 重拉 + 重连 WebView2 #2 | 内置 |
| 端口 / 状态 | ◉ | 小浮层显示 PID、端口、内存、运行时长 | 内置 |
| 设置 | ⚙ | 切到壳的设置页 | 内置 |
| （分隔）退出 | ✕ | 优雅退出 | 内置 |

**主项置顶且视觉权重最高**，因为「返回启动器」是最高频动作（占该菜单 70% 以上使用量）。同时保留双击直达这条更快路径——**菜单是给新用户的，双击是给老用户的**，两者并存不冲突。

#### 4.3 插件注册菜单项（关键扩展点）

菜单不能写死，否则你之前的插件体系就白做了。建议在插件 manifest 里加一段：

```json
{
  "id": "clean-logs",
  "fabMenu": {
    "label": "清理日志",
    "icon": "ui/icon.svg",
    "position": 20,
    "action": "launcher://plugin/clean-logs/run",
    "confirm": "确认清理 30 天前的日志？",
    "requireRunning": false
  }
}
```

宿主启动时扫描 `plugins/*/manifest.json`，按 `position` 合并进菜单，插在「设置」之前。动作统一走 `launcher://` URI 分发到插件宿主（进程外优先），**菜单层不知道也不关心插件是 DLL 还是 exe**。

约束：插件只能注册**一个**菜单项 + 一个二级子菜单（防止菜单膨胀）；图标必须自带 SVG/PNG，宿主不提供图标字体。

---

### 五、定位、跟随与多屏

| 场景 | 处理 |
|---|---|
| 初始位置 | 读 `config/fab.json` 的 `x,y`；首次运行放 WebView2 #2 右下角内 16px |
| 主窗体移动/缩放 | 监听主窗体 `WM_WINDOWPOSCHANGED` + WebView2 的 `WM_SIZE`，重算屏幕坐标后 `SetWindowPos(SWP_NOACTIVATE\|NOZORDER\|NOCOPYBITS)` |
| 拖拽换位置 | 按住 FabBtn 拖动超过 6px 阈值视为拖动（不与单击冲突：拖动不清除 armed 定时器即可，松开时若未超时就当单击处理——或者更干脆：**拖拽只在按钮边缘/长按 200ms 后生效**，避免误触） |
| 多显示器 | 用 `MonitorFromWindow` 取当前所在显示器的工作区做边界限制，禁止拖出可视区 |
| DPI 变化 | 响应 `WM_DPICHANGED`，按钮尺寸 = `48 * dpi/96`，图标用矢量或 2x PNG |
| 被盖住 | `TOPMOST` 只相对于本进程窗口组；不要设成系统级 TOPMOST（会盖住别的程序）。若确实被 Hermes 的全屏元素遮挡，用户可拖走或用托盘/热键兜底 |

---

### 六、可见性策略（什么时候显示）

```
显示条件 = (WebView2 #2 顶层可见) AND (当前 URL 属 Hermes 域) AND (网关就绪)
```

- 壳页面（shell.local）可见时 **隐藏** FabBtn，因为壳自己有完整导航，不需要第二个入口。
- Hermes 加载中的 loading 页也显示（用户可能卡在白屏，需要「重启网关」救命）。
- Hermes 崩溃/健康检查失败时，按钮换成**橙色警告态**，菜单里「重启网关」置顶——这是它最有价值的时刻。
- 切换时有 100ms 淡入淡出，不要突兀跳变。

---

### 七、通信与安全边界（再强调一次，这是最容易出事的地方）

```
FabBtn/FabMenu (原生 HWND)
      │  直接调用 native ShellService
      ▼
MainWnd ──► WebView2 #1 (shell.local)   host object ✅ 仅对 launcher.local
      │
      └─► WebView2 #2 (127.0.0.1:17520) host object ❌ 绝不暴露
```

- **FabBtn 根本不走 JS**，所以不存在「注入脚本被 Hermes 读到」的问题，方案 A 的安全隐患在此彻底消除。
- 唯一需要跨界的场景：如果将来想让 **Hermes 网页里的某个按钮也能唤起这个菜单**，那就只开一条单向 `postMessage` → 宿主，且消息白名单校验 origin == `http://127.0.0.1:17520`，并且**不允许通过这条通道传文件路径**。现阶段不建议做。
- 菜单项执行的文件操作一律经沙箱校验：限定在 `data/` 内、拒绝 `..` 与绝对路径、危险操作（清日志、删运行时）二次确认。

---

### 八、异常与兜底

1. **菜单打不开** → 托盘右键「打开启动器」+ 全局热键 `Ctrl+Shift+L`（必做，成本极低，是唯一的逃生口）。
2. **按钮拖出屏幕** → 启动时检测坐标是否落在任一显示器工作区内，否则重置到默认位。
3. **双击被识别成两次单击** → 几乎不可能（有定时器保护）；反向风险更大：**ARMED 阶段用户其实想单击却等不及**。所以压态视觉反馈 + 500ms 以内响应是关键，必要时提供「单击模式」配置。
4. **WebView2 #2 销毁/重建**（网关重启）期间 → 按钮置灰禁用，菜单项里除「设置」「退出」外全部 disable。
5. **高对比度/无障碍** → 按钮给 tooltip「Hermes 快捷菜单（单击展开，双击返回）」，并在设置里提供「隐藏悬浮按钮，只用热键」的开关。

---

### 九、接口定义

**C ABI（插件侧）**
```c
typedef struct {
    const char* id;
    const char* label;
    int position;
    const char* icon_svg;      // 内嵌字符串或资源名
    int (*run)(const char* args, void* ctx);   // 非阻塞，进度回调另传
    const char* (*confirm_msg)(void* ctx);     // NULL = 无需确认
} FabMenuItem;
```

**配置（data/config/fab.json）**
```json
{
  "enabled": true,
  "doubleClickTimeMs": 500,
  "singleClickMode": "deferred",
  "position": {"x": 1240, "y": 760},
  "menuOrder": ["back","logs","clean-logs","restart","status","settings","exit"]
}
```

**postMessage 协议（壳内 WebView2 #1 需要时）**
```json
{"type":"fab:open"}, {"type":"fab:navigate","target":"/home"}
```
仅限 `launcher.local` 源，其他源静默丢弃。

---

### 十、落地顺序

1. **Day 1**：FabBtn 原生分层窗口 + 单击/双击消歧 + 回到启动器（硬编码一个项）。验证手感。
2. **Day 2**：FabMenu 弹出窗口 + 6 个内置项 + 失焦关闭 + 键盘导航 + DPI/多屏。
3. **Day 3**：manifest 解析 + 插件菜单注册 + URI 分发 + 权限/沙箱校验。
4. **Day 4**：位置记忆、拖拽、主题暗色、动效、可见性策略、托盘与热键兜底。
5. **收尾**：UPX、杀软误报复测（新增的悬浮窗口 + TOPMOST 组合偶发报毒，记得签名）、高对比度与无障碍检查。

---

**需要我把第三节的消歧逻辑 + FabBtn/FabMenu 两个分层窗口的完整 Win32 代码（含 DPI 缩放、半透明圆角绘制、锚定计算、菜单项渲染与键盘导航）写出来吗？** 可以先告诉我你打算用纯 GDI/GDI+ 画，还是接 Direct2D（后者文字清晰度和动画更舒服，但多几十 KB 和初始化复杂度）。