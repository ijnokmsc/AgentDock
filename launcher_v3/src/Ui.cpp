// Ui.cpp —— 主题 / DPI / 绘制辅助实现 (GDI + GDI+)
#include "Ui.h"

#include <windowsx.h>
#include <gdiplus.h>
#include <algorithm>

#pragma comment(lib, "gdiplus.lib")

namespace ui {

const Theme& theme() {
    static Theme t;
    return t;
}

void EnablePerMonitorDpi() {
    // PerMonitorV2: 尺寸/坐标随 DPI 真实缩放, WM_DPICHANGED 可响应 (方案2 五)
    // 类型 (DPI_AWARENESS_CONTEXT / PROCESS_DPI_AWARENESS) 在部分 SDK 头中被
    // 条件编译遮蔽, 这里以动态声明 + 句柄值方式调用, 不依赖头内定义。
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        using FnSetCtx = BOOL(WINAPI*)(void*);
        auto fn = (FnSetCtx)(void*)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (fn) {
            void* perMonitorV2 = (void*)(INT_PTR)-1;   // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
            if (fn(perMonitorV2)) return;
        }
    }
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        using FnSetAware = HRESULT(WINAPI*)(int);
        auto fn = (FnSetAware)(void*)GetProcAddress(shcore, "SetProcessDpiAwareness");
        if (fn) fn(2);   // PROCESS_PER_MONITOR_DPI_AWARE
    }
}

UINT DpiOf(HWND hwnd) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        auto fn = reinterpret_cast<UINT(WINAPI*)(HWND)>(
            (void*)GetProcAddress(user32, "GetDpiForWindow"));
        if (fn) {
            UINT dpi = fn(hwnd);
            if (dpi) return dpi;
        }
    }
    HDC dc = GetDC(hwnd);
    UINT dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(hwnd, dc);
    return dpi ? dpi : 96;
}

int Scale(HWND hwnd, int v) { return MulDiv(v, (int)DpiOf(hwnd), 96); }

void FillRectARect(HDC hdc, int x, int y, int w, int h, COLORREF c) {
    RECT r{ x, y, x + w, y + h };
    HBRUSH b = CreateSolidBrush(c);
    FillRect(hdc, &r, b);
    DeleteObject(b);
}

namespace {
void RoundedPath(Gdiplus::GraphicsPath& p, int x, int y, int w, int h, int r) {
    Gdiplus::REAL rr = (Gdiplus::REAL)r * 2;
    p.AddArc((Gdiplus::REAL)x, (Gdiplus::REAL)y, rr, rr, 180, 90);
    p.AddArc((Gdiplus::REAL)(x + w - rr), (Gdiplus::REAL)y, rr, rr, 270, 90);
    p.AddArc((Gdiplus::REAL)(x + w - rr), (Gdiplus::REAL)(y + h - rr), rr, rr, 0, 90);
    p.AddArc((Gdiplus::REAL)x, (Gdiplus::REAL)(y + h - rr), rr, rr, 90, 90);
    p.CloseFigure();
}
}  // namespace

void FillRectRounded(HDC hdc, int x, int y, int w, int h, int r, COLORREF c) {
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath p;
    RoundedPath(p, x, y, w, h, r);
    Gdiplus::SolidBrush b(Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c)));
    g.FillPath(&b, &p);
}

void StrokeRectRounded(HDC hdc, int x, int y, int w, int h, int r, COLORREF c, int width) {
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath p;
    RoundedPath(p, x, y, w, h, r);
    Gdiplus::Pen pen(Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c)), (Gdiplus::REAL)width);
    g.DrawPath(&pen, &p);
}

HFONT MakeFont(int size, bool bold) {
    return CreateFontW(-size, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL,
                       FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH, L"Microsoft YaHei UI");
}

void DrawTextIn(HDC hdc, int x, int y, int w, int h, const std::wstring& s,
                COLORREF c, int size, bool bold, bool center, bool vcenter) {
    HFONT f = MakeFont(size, bold);
    HFONT old = (HFONT)SelectObject(hdc, f);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, c);
    RECT r{ x, y, x + w, y + h };
    UINT flags = DT_SINGLELINE | DT_END_ELLIPSIS
               | (center ? DT_CENTER : DT_LEFT)
               | (vcenter ? DT_VCENTER : DT_TOP);
    DrawTextW(hdc, s.c_str(), -1, &r, flags);
    SelectObject(hdc, old);
    DeleteObject(f);
}

void DrawDot(HDC hdc, int cx, int cy, int r, COLORREF c) {
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush b(Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c)));
    g.FillEllipse(&b, (Gdiplus::REAL)(cx - r), (Gdiplus::REAL)(cy - r),
                  (Gdiplus::REAL)(r * 2), (Gdiplus::REAL)(r * 2));
}

void DrawBolt(HDC hdc, int x, int y, int w, int h, COLORREF c) {
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath p;
    Gdiplus::REAL x0 = (Gdiplus::REAL)x, y0 = (Gdiplus::REAL)y;
    Gdiplus::REAL fw = (Gdiplus::REAL)w, fh = (Gdiplus::REAL)h;
    p.AddLine(x0 + fw * 0.62f, y0,               x0 + fw * 0.22f, y0 + fh * 0.52f);
    p.AddLine(x0 + fw * 0.22f, y0 + fh * 0.52f,   x0 + fw * 0.48f, y0 + fh * 0.52f);
    p.AddLine(x0 + fw * 0.48f, y0 + fh * 0.52f,   x0 + fw * 0.34f, y0 + fh);
    p.AddLine(x0 + fw * 0.34f, y0 + fh,           x0 + fw * 0.80f, y0 + fh * 0.44f);
    p.AddLine(x0 + fw * 0.80f, y0 + fh * 0.44f,   x0 + fw * 0.54f, y0 + fh * 0.44f);
    p.AddLine(x0 + fw * 0.54f, y0 + fh * 0.44f,   x0 + fw * 0.62f, y0);
    p.CloseFigure();
    Gdiplus::SolidBrush b(Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c)));
    g.FillPath(&b, &p);
}

bool FileExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

}  // namespace ui

// ---- 字符串转换 ----
namespace hs {

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

}  // namespace hs
