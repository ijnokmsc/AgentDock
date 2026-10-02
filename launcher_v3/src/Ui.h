// Ui.h —— 启动器 v3 公共头: 主题 / DPI / 绘制辅助 / 全局上下文
//
// 架构: docs/ADR-007-launcher-webview2-lifecycle.md
//   WebView2 #1 启动器页 (shell.local HTML, WebMessage 桥) / #2 Hermes 页 (无桥)
//   + FabBtn/FabMenu + 托盘/热键 + 原生 GDI 回退面板 (核心功能保留档)
//   核心层 (src/core|app|platform|storage) 0 改动接入, 构建★/更新★为产品核心。
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <objbase.h>   // GDI+ 头依赖 IStream 等 COM 声明
#include <string>
#include <vector>
#include <algorithm>

// NOMINMAX 下 GDI+ 头内使用裸 min/max, 统一在此引入 std::min/max
using std::min;
using std::max;

#include "FabConfig.h"
#include "HermesProc.h"
#include "DshProc.h"
#include "LogSink.h"

#include "app/Paths.h"
#include "core/Types.h"
#include "core/Launcher.h"
#include "core/Updater.h"
#include "core/PortableBuilder.h"
#include "core/ModelRegistry.h"
#include "core/PluginHost.h"
#include "core/ConfigRenderer.h"

namespace ui {

// ---- 主题 (暗色 + 青色强调, 对齐 app.ico 图标风格) ----
struct Theme {
    COLORREF bg        = RGB(0x0E, 0x11, 0x16);  // 窗体底
    COLORREF panel     = RGB(0x16, 0x1A, 0x22);  // 卡片/面板
    COLORREF panelHi   = RGB(0x1D, 0x22, 0x2D);  // 面板高亮 (hover)
    COLORREF border    = RGB(0x2A, 0x31, 0x40);
    COLORREF text      = RGB(0xE6, 0xEA, 0xF0);
    COLORREF muted     = RGB(0x8B, 0x93, 0xA7);
    COLORREF accent    = RGB(0x35, 0xD6, 0xF0);  // 青色强调
    COLORREF accentDim = RGB(0x1C, 0x6E, 0x82);
    COLORREF ok        = RGB(0x3F, 0xB9, 0x6C);
    COLORREF warn      = RGB(0xE5, 0xA1, 0x3F);
    COLORREF err       = RGB(0xE5, 0x50, 0x4F);
};
const Theme& theme();

// ---- DPI ----
UINT  DpiOf(HWND hwnd);
int   Scale(HWND hwnd, int v);
void  EnablePerMonitorDpi();

// ---- 绘制辅助 (GDI, 双缓冲由调用方负责) ----
void  FillRectARect(HDC hdc, int x, int y, int w, int h, COLORREF c);
void  FillRectRounded(HDC hdc, int x, int y, int w, int h, int r, COLORREF c);
void  StrokeRectRounded(HDC hdc, int x, int y, int w, int h, int r, COLORREF c, int width = 1);
void  DrawTextIn(HDC hdc, int x, int y, int w, int h, const std::wstring& s,
                 COLORREF c, int size, bool bold = false,
                 bool center = false, bool vcenter = true);
void  DrawDot(HDC hdc, int cx, int cy, int r, COLORREF c);
void  DrawBolt(HDC hdc, int x, int y, int w, int h, COLORREF c);
HFONT MakeFont(int size, bool bold);

// ---- 杂项 ----
bool  FileExists(const std::wstring& path);

}  // namespace ui

// 字符串转换 (UTF-8 <-> UTF-16, 定义在 Ui.cpp)
namespace hs {
std::string  WideToUtf8(const std::wstring& w);
std::wstring Utf8ToWide(const std::string& s);
}

// ---------------------------------------------------------------------------
// 全局应用上下文 (单实例, 定义在 main.cpp)
// ---------------------------------------------------------------------------
enum class RootState { Full, PendingBuild };   // 完整根 / 待构建 (空目录或半成品)

struct App {
    HINSTANCE       inst = nullptr;
    std::wstring    exeDirW;              // exe 所在目录 (= hs::Paths::portableRoot)
    std::wstring    root;                 // 内容根 (Full: 同 exeDir 或子目录; PendingBuild: exeDir)
    std::wstring    cfgPath;              // fab.json 绝对路径
    int             port = 8648;
    std::wstring    url;

    RootState       rootState = RootState::Full;

    hs::FabConfig   cfg;                  // 悬浮按钮配置 (fab.json)
    hs::HermesProc  proc;                 // 进程管理 (薄壳, 内部持 hs::Launcher)
    hs::DshProc     dshProc;              // DeepSeek Harness 进程管理 (ADR-008, 独立于 coreReady)
    hs::PluginHost  plugins;              // 插件宿主 (core 完整版)
    hs::Updater     updater;              // 组件更新 ★
    hs::PortableBuilder builder;          // 从0构建 ★
    hs::ModelRegistry* registry = nullptr; // models.json (构造需路径, 延迟 new)
    hs::AppSettings settings;             // studio/settings.json
    hs::ConfigRenderer* renderer = nullptr; // .env/config.yaml 渲染 (延迟 new)

    // 核心功能就绪标志 (PendingBuild 模式下 false, 构建完成后 InitCore 再置 true)
    bool coreReady = false;

    HWND            main = nullptr;       // 主面板
    HWND            fab  = nullptr;       // 悬浮按钮
    HWND            menu = nullptr;       // 浮动菜单

    UINT            wmTaskbarRestart = 0;
    bool            trayAdded = false;
    int             hotKeyId = 0x4848;
    bool            hotKeyOk = false;

    // ---- 路径助手 (PendingBuild 模式下回退到 exeDir/studio, 构建后迁移无需——
    //      settings 本来就存 exeDir/studio, OFFICIAL 布局两者同目录) ----
    std::wstring StudioDirW() const;      // <root>/studio (保证存在)
    std::wstring PluginDirW() const;
    std::wstring LogsDirW() const;

    // 核心层延迟初始化 (启动时 Full 根调用; 构建完成后再次调用)
    bool InitCore(std::wstring& err);
    void LoadSettings();                  // settings.json -> settings
    void SaveSettings();
};

extern App g_app;

// 各模块对外接口
namespace hs {

// 状态联动刷新: 主面板重绘 + 悬浮按钮重绘 + 托盘 tooltip (main.cpp)
void App_RefreshStatusUI();

void FabBtn_Create();
void FabBtn_UpdateVisual();          // 依据进程状态/自身状态重绘
void FabBtn_NotifyDpiChanged();
void FabBtn_Internal_MenuClosed();   // FabMenu 关闭后通知按钮恢复 IDLE

void FabMenu_Create();
void FabMenu_ShowAtFab();
void FabMenu_Hide();
bool FabMenu_IsVisible();

void MainWnd_Create(int nCmdShow);
void MainWnd_Show();                 // 显示并前置 (双击悬浮/托盘/热键)
void MainWnd_Navigate(int target);   // 0..9 原生页; 4 = Hermes 页签
void MainWnd_Refresh();
void MainWnd_Log(const std::wstring& line);
void MainWnd_RequestQuit();          // 真退出 (含停止 Hermes 确认)
void MainWnd_OpenHermes();           // WebView2 可用->嵌入页签; 否则系统浏览器
int  MainWnd_CurrentTab();           // 0 启动器 1 Hermes (FabBtn 可见性判定用)
void FabBtn_ApplyVisibility();       // 依据 cfg/页签/主窗可见性 显示或隐藏
void MainWnd_NativeEventSink(const std::string& json);   // Bridge 事件 -> 原生页面

void Tray_Add(HWND owner);
void Tray_Remove(HWND owner);
void Tray_UpdateTip();               // 按运行状态刷新 tooltip
void Tray_ShowMenu(HWND owner);      // 右键菜单

void MenuAction_OpenLogs();
void MenuAction_ShowStatus();

// WebMessage 桥 (Bridge.cpp): 命令分发 + 事件推送
void Bridge_Init();                  // UI 线程初始化
void Bridge_HandleJson(const std::string& jsonUtf8);   // WebView2 #1 收到页面消息
void Bridge_PushEvent(const std::string& jsonUtf8);    // 宿主 -> 页面 __hostEvent
void Bridge_FlushQueuedEvents();     // WebView2 Available 后冲刷排队事件
void Bridge_AutoUpdateScan();        // 启动时自动检查远端 (含 DSH 行, ADR-008)
void Bridge_OnShellNavigationCompleted();  // 启动器页导航完成后重推插件列表 (响应可能落在导航窗口期丢失)

// 托盘/菜单共用的插件动作收集 (FabMenu.cpp)
struct FabActionRef {
    std::wstring pluginId, pluginName, actionId, label, confirm;
    // HSPP v1.3: actions 元素可选 "type":"switch" + "default"
    bool isSwitch = false;       // 渲染为滑动开关, 点击翻转并回传 {"value":bool}
    bool switchOn = false;       // 宿主侧当前状态
    bool switchDefault = false;  // 声明的初始值 (无宿主记录时)
};
std::vector<FabActionRef> CollectPluginMenuActions();

constexpr UINT WM_TRAYICON = WM_APP + 30;
}  // namespace hs
