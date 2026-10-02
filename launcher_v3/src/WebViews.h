// WebViews.h —— WebView2 嵌入层 (方案2 架构的 #1/#2 双视图)
//
//   WebView2 #1  file:///<root>/shell.local/index.html   启动器自己的页 (WebMessage 桥)
//   WebView2 #2  http://127.0.0.1:<port>                 Hermes 页, 绝不挂任何宿主接口
//
// 可用性策略 (实测: 本机 Evergreen runtime 的浏览器进程 IPC/渲染损坏):
//   启动时异步探测 —— 创建 Environment/Controller 后发起导航, 10s 内收到任一
//   导航事件(NavigationStarting/Completed/SourceChanged)才判 Available;
//   否则 Unavailable, 主窗自动回退原生面板, 「打开 Hermes」回退系统浏览器。
//   browserExecutableFolder 优先 <root>\webview2 (便携 fixed 版), 其次 Evergreen。
#pragma once
#include <windows.h>
#include <string>

namespace hs {

enum class Wv2State { Probing, Available, Unavailable };

Wv2State Wv2_GetState();

// 主窗创建后调用 (异步; 结果通过 PostMessage(WM_WV2_STATE) 通知主窗刷新)
void Wv2_Init(HWND host, const std::wstring& root, int port);
constexpr UINT WM_WV2_STATE = WM_APP + 70;   // wParam: 1=Available 0=Unavailable

bool Wv2_ShowShell();                        // 前置 #1 (仅 Available)
bool Wv2_ShowHermes();                       // 前置 #2 (首次调用时导航; 网关重启后重导航)
void Wv2_NavigateHermes();                   // 网关重启后强制重新导航 #2
// #3 DSH 页 (ADR-008): token 每 boot 变化, desiredUrl 传当前带 token 的完整 URL;
// 与上次导航 URL 不同则重导航 (dsh 重启后自动换新 token)
bool Wv2_ShowDsh(const std::wstring& desiredUrl);
void Wv2_Hide();

// ---- 独立窗口 (拆分/合并, ADR-008) ----
// 把 agent 视图重挂到另一顶层窗口 (put_ParentWindow, 官方接口)。
// newHost = 目标顶层窗口 (拆分) 或主窗 (合并)。只重挂+设 Bounds, 可见性由调用方管。
bool Wv2_ReparentHermes(HWND newHost);
bool Wv2_ReparentDsh(HWND newHost);
void Wv2_SetHermesBounds(const RECT& rc);   // 独立窗 WM_SIZE 时同步
void Wv2_SetDshBounds(const RECT& rc);
bool Wv2_HasHermesView();                   // controller 已创建
bool Wv2_HasDshView();
void Wv2_NavigateDsh(const std::wstring& url);  // 独立窗首次挂载时主动导航

void Wv2_Resize(const RECT& contentRc);      // 内容区尺寸变化
void Wv2_PushEventJson(const std::string& jsonUtf8);   // __hostEvent(json) 推给 #1 (UI 线程)

}  // namespace hs
