// FabBtn.cpp —— 悬浮按钮 (方案2 第二节/第三节)
//
// 窗口形态: 48x48 (DPI 缩放) 分层窗口
//   WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE
//   点击不夺焦点 (用户在 Hermes 网页输入不被打断), 不进任务栏。
// 渲染: UpdateLayeredWindow + GDI+ 逐像素 alpha (圆角/微光/闪电)。
//
// 单击/双击消歧状态机 (方案2 3.2):
//   IDLE --LButtonDown--> ARMED (压态视觉, T=GetDoubleClickTime 可被配置覆盖)
//     ARMED + T 超时      --> 弹出 FabMenu
//     ARMED + 窗口内二击  --> 立即返回启动器 (不等鼠标抬起, 手感脆)
//   拖拽: 按住移动超过 6px(DPI 缩放) 进入拖动, 取消单击判定;
//         松手后位置钳制在显示器工作区内并写入 fab.json。
//
// 对旧实现缺陷的修正:
//   * 不再 WM_NCHITTEST=HTCAPTION (旧版导致点击被系统拖动吞掉, 消歧失效)
//   * 独立定位 + 记忆 (不再锚定主窗, 主窗最小化时按钮不会飞出屏幕)
//   * 启动时校验坐标落在某显示器工作区内, 否则重置默认位 (方案2 八.2)
#include "Ui.h"
#include "LogSink.h"

#include <gdiplus.h>
#include <shellapi.h>
#include <commctrl.h>
#include <algorithm>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")

namespace hs {
namespace {

using ui::Scale;
using ui::DpiOf;

constexpr wchar_t kClass[] = L"HermesV3FabBtn";
constexpr int  ID_TIMER_DCLICK = 1;
constexpr UINT MSG_MENU_CLOSED = WM_APP + 41;   // FabMenu -> FabBtn: 恢复 IDLE 视觉

enum class FabState { Idle, Armed };

struct FabState_ {
    FabState state = FabState::Idle;
    bool     dragging = false;
    POINT    pressPt{};              // 按下时屏幕坐标
    UINT     pressDpi = 96;
    int      dragThreshold = 6;      // 物理像素 (随 DPI)
    HWND     tip = nullptr;          // tooltip 控件
    bool     lastRenderOk = false;   // 最近一次 ULW 是否成功 (诊断)
};
FabState_ g;

int FabSize(HWND h) { return Scale(h, 48); }

// ---- 绘制: 在内存位图上画按钮, UpdateLayeredWindow 上屏 ----
void Render(HWND hwnd) {
    int sz = FabSize(hwnd);
    HDC screen = GetDC(hwnd);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = sz;
    bmi.bmiHeader.biHeight = -sz;            // 自上而下
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(hwnd, screen);
    if (!bmp) { DeleteDC(mem); return; }
    HGDIOBJ old = SelectObject(mem, bmp);

    Gdiplus::Graphics gfx(mem);
    gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    gfx.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    gfx.Clear(Gdiplus::Color(0, 0, 0, 0));   // 全透明底

    // 运行状态 -> 配色 (方案2 六: Failed 掉线橙色警告; Starting 保持常规态)
    auto st = g_app.proc.Query();
    bool warn = PhaseIsWarn(st.phase);
    bool armed = (g.state == FabState::Armed);

    int inset = armed ? Scale(hwnd, 2) : 0;  // ARMED: 内缩 2px 模拟 scale 0.92
    Gdiplus::REAL radius = (Gdiplus::REAL)Scale(hwnd, 13);
    Gdiplus::Rect body(inset, inset, sz - inset * 2, sz - inset * 2);

    DWORD bodyArgb = warn ? (armed ? 0xF0B45C16u : 0xF09A4A0Eu)    // 橙
                          : (armed ? 0xEC232A3Au : 0xEC1A1F2Bu);   // 深蓝黑
    Gdiplus::Color bodyColor(bodyArgb);
    Gdiplus::SolidBrush bodyBrush(bodyColor);
    Gdiplus::GraphicsPath round;
    round.AddArc((Gdiplus::REAL)body.X, (Gdiplus::REAL)body.Y, radius * 2, radius * 2, 180, 90);
    round.AddArc((Gdiplus::REAL)(body.GetRight() - radius * 2), (Gdiplus::REAL)body.Y, radius * 2, radius * 2, 270, 90);
    round.AddArc((Gdiplus::REAL)(body.GetRight() - radius * 2), (Gdiplus::REAL)(body.GetBottom() - radius * 2), radius * 2, radius * 2, 0, 90);
    round.AddArc((Gdiplus::REAL)body.X, (Gdiplus::REAL)(body.GetBottom() - radius * 2), radius * 2, radius * 2, 90, 90);
    round.CloseFigure();
    gfx.FillPath(&bodyBrush, &round);

    // 边框: accent 微光 (警告态为橙色)
    Gdiplus::Color edge = warn ? Gdiplus::Color(0xB8, 0xE5, 0xA1, 0x3F)
                               : Gdiplus::Color(0xB8, 0x35, 0xD6, 0xF0);
    Gdiplus::Pen pen(edge, (Gdiplus::REAL)max(1.5f, sz * 0.032f));
    gfx.DrawPath(&pen, &round);

    // 中心闪电 (对齐 app.ico 视觉)
    int iw = (int)(sz * 0.46f), ih = (int)(sz * 0.52f);
    int ix = (sz - iw) / 2, iy = (sz - ih) / 2;
    if (armed) { ix += Scale(hwnd, 1); iy += Scale(hwnd, 1); }   // 压态轻微下沉
    Gdiplus::Color bolt = warn ? Gdiplus::Color(0xFF, 0xFF, 0xB0, 0x2E)
                               : Gdiplus::Color(0xFF, 0x35, 0xD6, 0xF0);
    if (armed) bolt = Gdiplus::Color::White;
    {
        Gdiplus::GraphicsPath boltPath;
        Gdiplus::REAL x0 = (Gdiplus::REAL)ix, y0 = (Gdiplus::REAL)iy;
        Gdiplus::REAL w = (Gdiplus::REAL)iw, h = (Gdiplus::REAL)ih;
        boltPath.AddLine(x0 + w * 0.62f, y0,               x0 + w * 0.22f, y0 + h * 0.52f);
        boltPath.AddLine(x0 + w * 0.22f, y0 + h * 0.52f,   x0 + w * 0.48f, y0 + h * 0.52f);
        boltPath.AddLine(x0 + w * 0.48f, y0 + h * 0.52f,   x0 + w * 0.34f, y0 + h);
        boltPath.AddLine(x0 + w * 0.34f, y0 + h,           x0 + w * 0.80f, y0 + h * 0.44f);
        boltPath.AddLine(x0 + w * 0.80f, y0 + h * 0.44f,   x0 + w * 0.54f, y0 + h * 0.44f);
        boltPath.AddLine(x0 + w * 0.54f, y0 + h * 0.44f,   x0 + w * 0.62f, y0);
        boltPath.CloseFigure();
        Gdiplus::SolidBrush boltBrush(bolt);
        gfx.FillPath(&boltBrush, &boltPath);
    }

    // 注意顺序: ULW 要求 hdcSrc 选中的就是待呈现位图 (尺寸须与窗口一致)。
    // 旧实现在 ULW 之前就 SelectObject 恢复默认 1x1 位图 -> ULW 恒报 err=31,
    // 悬浮球自 v3 起从未渲染出来 (存量 bug)。
    POINT dst{ 0, 0 }, src{ 0, 0 };
    SIZE  size{ sz, sz };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    BOOL ulwOk = UpdateLayeredWindow(hwnd, nullptr, nullptr, &size, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, old);
    if (!ulwOk)   // 悬浮球不可见的定位线索: ULW 失败原因 + 尺寸
        logSink().Log(LogLevel::Error, L"[fab] ULW failed err=" + std::to_wstring(GetLastError())
                                      + L" sz=" + std::to_wstring(sz));
    g.lastRenderOk = (ulwOk != FALSE);

    DeleteObject(bmp);
    DeleteDC(mem);
}

// 坐标是否落在任一显示器工作区内 (方案2 八.2)
bool PointInAnyWorkArea(int x, int y) {
    POINT pt{ x, y };
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONULL);
    if (!mon) return false;
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    return x >= mi.rcWork.left && x <= mi.rcWork.right && y >= mi.rcWork.top && y <= mi.rcWork.bottom;
}

void ClampToWorkArea(int& x, int& y, int w, int h) {
    POINT pt{ x + w / 2, y + h / 2 };
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y < mi.rcWork.top)  y = mi.rcWork.top;
    if (x + w > mi.rcWork.right)  x = mi.rcWork.right - w;
    if (y + h > mi.rcWork.bottom) y = mi.rcWork.bottom - h;
}

void SaveFabPos(HWND h) {
    RECT rc; GetWindowRect(h, &rc);
    g_app.cfg.x = rc.left;
    g_app.cfg.y = rc.top;
    g_app.cfg.Save(g_app.cfgPath);
}

void GoToLauncher() {
    MainWnd_Show();          // 双击直达 (方案2 3.2: 不等抬起)
    MainWnd_Navigate(0);     // 切到「启动器」页签
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        // tooltip (方案2 八.5 无障碍提示)
        g.tip = CreateWindowExW(0, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP,
                                CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                h, nullptr, g_app.inst, nullptr);
        TOOLINFOW ti{ sizeof(ti), TTF_SUBCLASS, h, 1 };
        ti.lpszText = const_cast<LPWSTR>(L"AgentDock 快捷菜单 (单击展开, 双击返回启动器; 可拖动)");
        RECT rc; GetClientRect(h, &rc);
        ti.rect = rc;
        SendMessageW(g.tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        SendMessageW(g.tip, TTM_SETMAXTIPWIDTH, 0, 400);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);   // 实际绘制在 ULW, 这里仅满足系统
        EndPaint(h, &ps);
        Render(h);
        return 0;
    }
    // ---- 消歧状态机 (方案2 3.3) ----
    case WM_LBUTTONDOWN: {
        if (FabMenu_IsVisible()) { FabMenu_Hide(); return 0; }   // 菜单开着: 点按钮=关闭
        SetCapture(h);                                            // 拖拽期间继续收鼠标消息
        g.pressDpi = DpiOf(h);
        g.dragThreshold = max(4, (int)(6 * g.pressDpi / 96.0));
        POINT pt; GetCursorPos(&pt);
        g.pressPt = pt;
        if (!g_app.cfg.immediateClick && g.state == FabState::Armed) {
            // 时间窗内第二次按下 -> 双击: 立即返回启动器
            KillTimer(h, ID_TIMER_DCLICK);
            g.state = FabState::Idle;
            Render(h);
            GoToLauncher();
            ReleaseCapture();
            g.dragging = false;
            return 0;
        }
        if (g_app.cfg.immediateClick) {
            // 立即单击模式: 弹菜单, 不做双击判定 (方案2 3.2 可选增强)
            ReleaseCapture();
            g.state = FabState::Idle;
            Render(h);
            FabMenu_ShowAtFab();
            return 0;
        }
        g.state = FabState::Armed;
        Render(h);                                                // ARMED 压态, 消除迟滞感
        SetTimer(h, ID_TIMER_DCLICK, g_app.cfg.ClickWindowMs(), nullptr);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (w & MK_LBUTTON && !g.dragging) {
            POINT pt; GetCursorPos(&pt);
            int dx = pt.x - g.pressPt.x, dy = pt.y - g.pressPt.y;
            if (dx * dx + dy * dy >= g.dragThreshold * g.dragThreshold) {
                // 超过阈值 -> 拖拽, 取消单击/双击判定
                KillTimer(h, ID_TIMER_DCLICK);
                g.state = FabState::Idle;
                g.dragging = true;
                Render(h);
            }
        }
        if (g.dragging) {
            POINT pt; GetCursorPos(&pt);
            int sz = FabSize(h);
            int x = pt.x - sz / 2, y = pt.y - sz / 2;
            ClampToWorkArea(x, y, sz, sz);
            SetWindowPos(h, HWND_TOPMOST, x, y, 0, 0,
                         SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        ReleaseCapture();
        if (g.dragging) {
            g.dragging = false;
            SaveFabPos(h);                                        // 位置记忆 (方案2 五)
        }
        return 0;
    }
    case WM_TIMER: {
        if (w == ID_TIMER_DCLICK) {
            // 超时且无第二次点击 -> 弹菜单
            KillTimer(h, ID_TIMER_DCLICK);
            g.state = FabState::Idle;
            Render(h);
            FabMenu_ShowAtFab();
        }
        return 0;
    }
    case WM_RBUTTONUP:
        // 右键 = 单击等效 (直达菜单), 方便鼠标用户
        FabMenu_ShowAtFab();
        return 0;
    case WM_DPICHANGED: {
        // DPI 变化: 以建议矩形重设尺寸并重绘 (方案2 五)
        auto* sug = (RECT*)l;
        int sz = MulDiv(48, HIWORD(w), 96);
        SetWindowPos(h, nullptr, sug->left, sug->top, sz, sz,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        Render(h);
        return 0;
    }
    case MSG_MENU_CLOSED:
        g.state = FabState::Idle;
        Render(h);
        return 0;
    case WM_DESTROY:
        if (g.tip) { DestroyWindow(g.tip); g.tip = nullptr; }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

void FabBtn_Create() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = g_app.inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    // 初始位置: 配置记忆 -> 校验工作区 -> 否则主屏右下角内 16px (方案2 五)
    HMONITOR mon = MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);

    UINT dpi = 96;
    HDC dc = GetDC(nullptr);
    dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    int sz = MulDiv(48, dpi, 96);

    int x = g_app.cfg.x, y = g_app.cfg.y;
    bool valid = (x >= 0 && y >= 0 && PointInAnyWorkArea(x, y));
    if (!valid) {
        x = mi.rcWork.right - sz - MulDiv(16, dpi, 96);
        y = mi.rcWork.bottom - sz - MulDiv(16, dpi, 96);
        g_app.cfg.x = x; g_app.cfg.y = y;
    }

    g_app.fab = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE,
        kClass, L"", WS_POPUP,
        x, y, sz, sz,
        nullptr, nullptr, g_app.inst, nullptr);
    ShowWindow(g_app.fab, g_app.cfg.enabled ? SW_SHOWNOACTIVATE : SW_HIDE);
    Render(g_app.fab);
    logSink().Log(LogLevel::Info, L"[fab] create: dpi=" + std::to_wstring(dpi)
        + L" sz=" + std::to_wstring(sz) + L" pos=" + std::to_wstring(x) + L"," + std::to_wstring(y)
        + L" renderOk=" + (g.lastRenderOk ? L"1" : L"0")
        + L" enabled=" + (g_app.cfg.enabled ? L"1" : L"0"));
    // ShowWindow 刚完成时 ULW 可能被 DWM 忽略; 走一次同步 WM_PAINT 确保首帧上屏
    InvalidateRect(g_app.fab, nullptr, TRUE);
    UpdateWindow(g_app.fab);
}

void FabBtn_UpdateVisual() {
    if (!g_app.fab || !IsWindowVisible(g_app.fab)) return;
    Render(g_app.fab);
}

// 可见性策略 (ADR-007): 仅当「Hermes」页签激活且主窗可见时显示 ——
// 启动器页签自身就是完整 UI, 不需要第二个入口 (方案2 六的收敛版)。
void FabBtn_ApplyVisibility() {
    if (!g_app.fab) return;
    // 任意页签常驻 (插件快捷区进入悬浮菜单后, 启动器页签也要能一键触达;
    // 双击回启动器在启动器页签下是无操作, 无副作用)。
    bool show = g_app.cfg.enabled
             && g_app.main && IsWindowVisible(g_app.main)
             && !g_app.cfg.hideWhenMainVisible;
    if (show) {
        ShowWindow(g_app.fab, SW_SHOWNOACTIVATE);
        Render(g_app.fab);
    } else {
        FabMenu_Hide();
        ShowWindow(g_app.fab, SW_HIDE);
    }
}

void FabBtn_NotifyDpiChanged() {
    if (!g_app.fab) return;
    int sz = FabSize(g_app.fab);
    RECT rc; GetWindowRect(g_app.fab, &rc);
    int x = rc.left, y = rc.top;
    ClampToWorkArea(x, y, sz, sz);
    SetWindowPos(g_app.fab, nullptr, x, y, sz, sz, SWP_NOACTIVATE | SWP_NOZORDER);
    Render(g_app.fab);
}

}  // namespace hs

// FabMenu 淡出完成后恢复按钮 IDLE 视觉 (跨编译单元, 声明于 Ui.h)
namespace hs {
void FabBtn_Internal_MenuClosed() {
    if (g_app.fab) PostMessageW(g_app.fab, MSG_MENU_CLOSED, 0, 0);
}
}
