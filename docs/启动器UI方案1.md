以下是基于 **方案 B（独立悬浮窗口）+ C（系统托盘兜底）** 的完整设计方案，你可以直接照着实现。

---

## 启动器集成 Hermes Web UI 设计方案

### 一、整体架构

```
┌─────────────────────────────────────────────┐
│              主窗口 (HWND_MAIN)              │
│  ┌───────────────────────────────────────┐  │
│  │  WebView2 控件（填满客户区）          │  │
│  │  Navigate → http://127.0.0.1:17520    │  │
│  │  Host Object → 挂载插件接口           │  │
│  └───────────────────────────────────────┘  │
│                                              │
│  ┌────────┐  ┌──────────────────────────┐   │
│  │悬浮窗HWND│  │  系统托盘图标 (Shell_NotifyIcon)│  │
│  └────────┘  └──────────────────────────┘   │
└─────────────────────────────────────────────┘

子进程：Hermes 网关进程 (PID xxxx) ── 监听 127.0.0.1:17520 / 8648
子进程：Web UI 进程 (PID 22064) ────────────────────
```

**核心原则：壳归壳，内容归内容。** 主窗口只负责托管 WebView2，不关心网页内容是什么；悬浮窗和托盘是独立 HWND，与 WebView2 物理隔离。

---

### 二、主窗口设计

#### 2.1 窗口风格

```cpp
HWND hMain = CreateWindowExW(
    0,
    L"LauncherMainClass",
    L"HermesStudio",
    WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,  // ← 必须加 WS_CLIPCHILDREN
    CW_USEDEFAULT, CW_USEDEFAULT,
    1280, 800,
    nullptr, nullptr, hInst, nullptr);
```

- `WS_CLIPCHILDREN` 保证 WebView2 重绘时不会把宿主窗口其他内容擦掉。
- 无原生标题栏（可选）：`WS_POPUP | WS_THICKFRAME | WS_CAPTION` 配合自绘标题栏，视觉更干净。

#### 2.2 WebView2 初始化

```cpp
// 1. 创建 WebView2 Environment，指定便携 userDataFolder
CreateCoreWebView2EnvironmentWithOptions(
    nullptr,                           // 使用系统自带 WebView2
    L".\\HermesPortable\\WebViewData", // 便携目录，不写 C 盘
    nullptr,
    Callback<IAsyncOperation<ICoreWebView2Environment*>>(
        [&](ICoreWebView2Environment* env, HRESULT err) {
            env->CreateCoreWebView2Host(hMain,
                Callback<IAsyncOperation<ICoreWebView2Controller*>>(
                    [&](ICoreWebView2Controller* ctrl, HRESULT) {
                        // 2. 获取 WebView 接口
                        ctrl->get_CoreWebView2(&webview);

                        // 3. 调整大小铺满客户区
                        RECT rc; GetClientRect(hMain, &rc);
                        ctrl->put_Bounds(rc);

                        // 4. 挂载 Host Object（插件通信入口）
                        webview->AddHostObjectToScript(
                            L"launcher",
                            MakeRawResource(new LauncherHostObject()));

                        // 5. 导航到 Hermes（在健康检查通过后）
                        webview->Navigate(L"http://127.0.0.1:17520");

                        return S_OK;
                    }).Get());
            return S_OK;
        }).Get());
```

**关键点：**
- `userDataFolder` 指向便携目录下的子文件夹，退出时可选删除（`SHFileOperation`）。
- `AddHostObjectToScript` 暴露的接口名 `window.chrome.webview.hostObjects.launcher`，Hermes 页面内的 JS 可以直接调用宿主方法。

#### 2.3 窗口大小调整

```cpp
case WM_SIZE:
    if (webviewController) {
        RECT rc; GetClientRect(hWnd, &rc);
        webviewController->put_Bounds(rc);
    }
    // 同步更新悬浮窗位置
    UpdateFloatWindowPos();
    break;
```

---

### 三、悬浮窗口设计

#### 3.1 创建悬浮窗

```cpp
HWND hFloat = CreateWindowExW(
    WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE,
    L"LauncherFloatClass",
    L"返回启动器",
    WS_POPUP,
    0, 0, 52, 52,
    hMain, nullptr, hInst, nullptr);

// 圆角 + 半透明
SetWindowRgn(hFloat, CreateRoundRectRgn(0, 0, 52, 52, 16, 16), TRUE);
SetLayeredWindowAttributes(hFloat, 0, 220, LWA_ALPHA);
```

- `WS_EX_NOACTIVATE`：**关键**——点击悬浮窗不会夺走 WebView2 的焦点，用户正在网页里打字不会被中断。
- `WS_EX_TOOLWINDOW` 让悬浮窗不出现在任务栏。
- `WS_EX_LAYERED` 配合 `SetLayeredWindowAttributes` 实现半透明。

#### 3.2 悬浮窗位置

```cpp
void UpdateFloatWindowPos() {
    RECT rcMain;
    GetWindowRect(hMain, &rcMain);
    int x = rcMain.right - 64;
    int y = rcMain.bottom - 64;
    SetWindowPos(hFloat, HWND_TOPMOST, x, y, 52, 52,
        SWP_NOACTIVATE | SWP_NOSENDCHANGING);
}
```

始终锚定主窗口右下角，随主窗口移动/缩放同步更新（`WM_MOVE` / `WM_SIZE` 里调用）。

#### 3.3 悬浮窗绘制（自绘图标）

```cpp
case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hWnd, &ps);

    // 背景
    HBRUSH hBrush = CreateSolidBrush(RGB(40, 44, 52));
    FillRect(hdc, &ps.rcPaint, hBrush);
    DeleteObject(hBrush);

    // 绘制图标（你的 logo 或一个 🏠 字符）
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    SelectObject(hdc, CreateFontW(24, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Emoji"));
    TextOutW(hdc, 14, 10, L"🏠", 1);

    EndPaint(hWnd, &ps);
    break;
}
```

#### 3.4 悬浮窗交互

```cpp
case WM_LBUTTONDOWN:
    // 显示启动器面板（见下文「面板切换」）
    ShowLauncherPanel();
    break;

case WM_MOUSEENTER:  // 需 TrackMouseEvent 触发
    // 悬停高亮
    SetLayeredWindowAttributes(hWnd, 0, 255, LWA_ALPHA);
    InvalidateRect(hWnd, nullptr, TRUE);
    break;

case WM_MOUSELEAVE:
    SetLayeredWindowAttributes(hWnd, 0, 220, LWA_ALPHA);
    InvalidateRect(hWnd, nullptr, TRUE);
    break;
```

---

### 四、系统托盘设计

```cpp
NOTIFYICONDATAW nid = {};
nid.cbSize = sizeof(nid);
nid.hWnd = hMain;
nid.uID = 1;
nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
nid.uCallbackMessage = WM_TRAYICON;
nid.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP));
wcscpy_s(nid.szTip, L"HermesStudio - 运行中");
Shell_NotifyIconW(NIM_ADD, &nid);
```

托盘菜单项：
- **打开启动器面板** → `ShowLauncherPanel()`
- **打开 Hermes** → `webview->Navigate(L"http://127.0.0.1:17520")`
- **清理日志** → 直接调插件逻辑
- **退出** → 杀子进程 + 清理 + 退出

```cpp
case WM_TRAYICON:
    if (lParam == WM_RBUTTONUP) {
        // 弹出右键菜单
        POINT pt; GetCursorPos(&pt);
        HMENU hMenu = CreatePopupMenu();
        AppendMenuW(hMenu, MF_STRING, ID_TRAY_OPEN, L"打开启动器");
        AppendMenuW(hMenu, MF_STRING, ID_TRAY_CLEANUP, L"清理日志");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"退出");
        SetForegroundWindow(hMain); // 解决菜单不消失的 bug
        TrackPopupMenu(hMenu, TPM_BOTTOMALIGN, pt.x, pt.y, 0, hMain, nullptr);
    }
    break;
```

---

### 五、面板切换逻辑

悬浮窗/托盘点击后，需要**暂时覆盖 Hermes 页面**，展示启动器自身的内容面板（日志、插件、设置等）。

**做法：用一个全屏子窗口盖住 WebView2。**

```cpp
HWND hPanel = nullptr; // 启动器面板窗口

void ShowLauncherPanel() {
    if (!hPanel) {
        hPanel = CreateWindowExW(
            0, L"LauncherPanelClass", nullptr,
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
            0, 0, 0, 0,
            hMain, nullptr, hInst, nullptr);

        // 在 hPanel 里用 FLTK / 自绘 / 另一个 WebView2 渲染你的 UI
        // 推荐：保留你现在的 FLTK 代码，但让它 render 到 hPanel 的 HDC 上
        // 或者更简单：hPanel 再嵌一个 WebView2，NavigateToString 喂本地 HTML
    }
    SetWindowPos(hPanel, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    ShowWindow(hPanel, SW_SHOW);
}

void HideLauncherPanel() {
    if (hPanel) {
        ShowWindow(hPanel, SW_HIDE);
        SetWindowPos(hPanel, HWND_BOTTOM, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}
```

切换逻辑：
- 悬浮窗点击 → `ShowLauncherPanel()` → 展示启动器 UI → 用户操作完后点击"返回 Hermes" → `HideLauncherPanel()` → 露出下面的 WebView2
- WebView2 始终在跑，Hermes 的会话状态不会丢

---

### 六、插件体系集成

#### 6.1 插件宿主对象（Host Object）

```cpp
class LauncherHostObject : public IUnknown {
    // 暴露给 JS 的接口
    HRESULT STDMETHODCALLTYPE InvokeMethod(
        const wchar_t* pluginName,
        const wchar_t* methodName,
        const wchar_t* argsJson,
        wchar_t** outResultJson) {
        // 查找插件 → 调用对应方法 → 返回 JSON 结果
        Plugin* p = PluginManager::Find(pluginName);
        std::wstring result = p->Call(methodName, argsJson);
        *outResultJson = _wcsdup(result.c_str());
        return S_OK;
    }

    // 例如：清理日志
    HRESULT STDMETHODCALLTYPE CleanupLogs(int keepDays, int* deletedCount) {
        int count = PluginManager::Get("CleanupLogs")->Execute(keepDays);
        *deletedCount = count;
        return S_OK;
    }
};
```

#### 6.2 Hermes 页面内调用插件

```javascript
// Hermes 前端页面中的 JS（通过注入或用户手动写）
async function cleanupLogs(days) {
    const launcher = window.chrome.webview.hostObjects.sync.launcher;
    const count = await launcher.CleanupLogs(days);
    console.log(`已清理 ${count} 个日志文件`);
}
```

#### 6.3 纯后端插件（无界面）

清理日志、端口检测、杀进程等插件完全不需要改，启动器通过托盘菜单/快捷键触发即可：

```cpp
// 托盘菜单 → 清理日志
case ID_TRAY_CLEANUP:
    int deleted;
    webview->ExecuteScript(
        L"window.chrome.webview.hostObjects.sync.launcher.CleanupLogs(7)",
        Callback<IAsyncOperation<LPWSTR>>().Get());
    break;
```

---

### 七、进程启动时序

```
┌─────────┐     ┌──────────┐     ┌───────────┐     ┌──────────┐
│ 启动器  │────▶│ 起 Hermes │────▶│ 轮询健康检查│────▶│WebView2   │
│ 主进程  │     │ 子进程    │     │ /health    │