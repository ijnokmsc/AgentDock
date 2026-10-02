// FabMenu.cpp —— 浮动菜单 (方案2 第四节)
//
// 形态: 独立分层弹出窗口 (WS_EX_TOOLWINDOW|LAYERED|NOACTIVATE), 不属于
//       主窗 HWND 树, 不抢焦点; UpdateLayeredWindow 逐像素 alpha 绘制
//       (圆角面板 + 模拟投影 + 120ms 淡入 / 80ms 淡出, 方案2 4.1)。
// 锚定: FabBtn 上方 8px 居中; 超出工作区上沿则翻到下方。
// 关闭: ESC / 选中项 / 点击外部 / 启动器切页。
// 键盘: ↑↓ 导航, Enter 执行 —— NOACTIVATE 窗口收不到键盘消息, 旧实现的
//       WM_KEYDOWN 是死代码; 这里用 WH_KEYBOARD_LL 在菜单期间接管 (方案2 4.1)。
//       点击外部关闭同理用 WH_MOUSE_LL (WM_ACTIVATEAPP 对 NOACTIVATE 永不触发)。
// 菜单项: 内置项 (含按运行状态动态的 启动/停止/重启) + 插件动作
//       (core PluginHost uiEntries.actions, 插在「设置」之前, 方案2 4.3)。
#include "Ui.h"
#include "TaskRunner.h"
#include "core/DshRuntime.h"
#include <windowsx.h>

#include <gdiplus.h>
#include <nlohmann/json.hpp>

#pragma comment(lib, "gdiplus.lib")

namespace hs {
void FabBtn_Internal_MenuClosed();   // FabBtn.cpp
}

namespace hs {
namespace {

using ui::Scale;

constexpr wchar_t kClass[] = L"HermesV3FabMenu";
constexpr UINT MSG_KEY_NAV     = WM_APP + 51;   // LL 键盘钩子 -> 菜单
constexpr UINT MSG_OUTSIDE     = WM_APP + 52;   // LL 鼠标钩子 -> 菜单
constexpr UINT MSG_FADE        = WM_APP + 53;   // 淡入淡出定时器
constexpr UINT MSG_RESULT      = WM_APP + 54;   // 插件动作异步结果 (TaskRunner 回投)
constexpr int  ID_TIMER_FADE   = 2;
constexpr int  ID_TIMER_RESULT = 3;             // 结果行展示 4s 后自动清除

enum ActionKind {
    ACT_AGENT_START, ACT_AGENT_STOP, ACT_AGENT_RESTART, ACT_AGENT_OPEN,
    ACT_PLUGIN, ACT_PLUGIN_SWITCH,
    ACT_PLUGIN_GROUP,                // HSPP v1.3: 插件名分组头 (不可点)
    ACT_SETTINGS, ACT_EXIT,
};

struct Item {
    std::wstring label;
    std::wstring glyph;
    ActionKind   kind = ACT_AGENT_START;
    bool         primary = false;    // 主项: 加粗 + accent 竖条 (方案2 4.2)
    bool         danger = false;     // 退出等破坏性项
    std::wstring pluginId, actionId; // ACT_PLUGIN*: 插件动作定位
    std::wstring confirm;            // 非空 = 执行前确认 (插件声明)
    bool         isSwitch = false;   // switch 动作
    bool         switchOn = false;   // switch 当前状态 (渲染 ◉/○)
    // Agent 项 (ACT_AGENT_*): 目标 agent 与名称 (错误弹窗/日志用)
    WebAgentProc* agent = nullptr;
    std::wstring agentName;
    int  stateColor = 0;             // 图标配色: 0=默认 1=运行中(绿) 2=掉线(橙)
    bool enabled = true;
    bool interactive() const { return kind != ACT_PLUGIN_GROUP; }
};

struct MenuState {
    std::vector<Item> items;
    int  hover = -1, sel = -1;
    bool visible = false;
    bool fadingOut = false;
    int  alpha = 0;
    std::wstring resultLine;         // 底部结果行: 插件动作执行中/完成反馈
    bool resultOk = true;            // 结果行配色: 绿=成功 红=失败 灰=执行中
    bool resultBusy = false;
    int  resultH = 0;                // 结果行像素高 (随行数变化, RenderCache 计算)
    // 渲染缓存 (内容不变时动画只改整体 alpha)
    HBITMAP cacheBmp = nullptr;
    SIZE   cacheSize{};
    bool   cacheDirty = true;
    // LL 钩子
    HHOOK  kbHook = nullptr, msHook = nullptr;
    // 布局 (物理像素)
    int  W = 0, IH = 0, PAD = 0, SH = 0;   // 面板宽/项高/内边距/阴影边距
};
MenuState g;

int ItemCount() { return (int)g.items.size(); }
int PanelHeight() { return ItemCount() * g.IH + g.PAD * 2 + g.resultH; }

// 结果行内容设置 (执行中/成功/失败); 超过 4 行裁剪
void SetResult(HWND h, const std::wstring& text, bool ok, bool busy) {
    g.resultLine = text;
    g.resultOk = ok;
    g.resultBusy = busy;
    int lines = 1;
    for (wchar_t c : g.resultLine) if (c == L'\n') ++lines;
    if (lines > 4) {
        int seen = 0;
        std::wstring trimmed;
        for (wchar_t c : g.resultLine) {
            if (c == L'\n' && ++seen == 4) break;
            trimmed += c;
        }
        g.resultLine = trimmed;
        lines = 4;
    }
    g.resultH = lines * Scale(h, 16) + Scale(h, 8);
}

void RenderCache(HWND h);   // 前置声明 (结果行增删/开关翻转后整窗重排)
void Present(HWND h, BYTE alpha);
void DoFadeStep(HWND h);    // 淡入淡出单步 (WndProc 内引用)
void PushAgentBlock(const wchar_t* name, WebAgentProc& p, WebAgentProc::Status s);
Item MakeAgentItem(ActionKind kind, WebAgentProc& p, WebAgentProc::Status s,
                   const std::wstring& label, bool primary, bool danger);

// 布局重算 + 重绘 (结果行出现/清除会改面板高度, 窗口随之 resize)
void RefreshMenu(HWND h) {
    g.cacheDirty = true;
    RenderCache(h);
    SetWindowPos(h, nullptr, 0, 0, g.W + g.SH * 2, PanelHeight() + g.SH * 2,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    Present(h, (BYTE)g.alpha);
}

void BuildItems() {
    g.items.clear();
    auto st  = g_app.proc.Query();
    auto dst = g_app.dshProc.Query();

    // 掉线警告置顶 (方案2 六: 警告时刻是它最有价值的时候)
    if (st.phase == Phase::Failed)
        g.items.push_back(MakeAgentItem(ACT_AGENT_RESTART, g_app.proc, st,
                                        L"重启 Hermes (已掉线)", true, true));
    if (dst.phase == Phase::Failed && hs::dsh::installed())
        g.items.push_back(MakeAgentItem(ACT_AGENT_RESTART, g_app.dshProc, dst,
                                        L"重启 DSH (已掉线)", true, true));

    // 每 Agent 一个精简块: 运行中 = [打开][停止] / 启动中 = [停止(中断)] / 其余 = [启动]。
    // 不再放 返回启动器(双击球=回启动器)/查看日志/端口状态 —— 精简由多 Agent 共用这份菜单。
    PushAgentBlock(L"Hermes", g_app.proc, st);
    if (hs::dsh::installed())
        PushAgentBlock(L"DSH", g_app.dshProc, dst);

    // 插件快捷区 (HSPP v1.3: 分组头 + 按钮/开关)
    {
        std::wstring lastPlugin;
        for (auto& ref : CollectPluginMenuActions()) {
            if (ref.pluginName != lastPlugin) {
                Item head;
                head.kind = ACT_PLUGIN_GROUP;
                head.label = L"◆ " + ref.pluginName;
                g.items.push_back(std::move(head));
                lastPlugin = ref.pluginName;
            }
            Item it;
            it.kind = ref.isSwitch ? ACT_PLUGIN_SWITCH : ACT_PLUGIN;
            it.label = ref.label;
            it.pluginId = ref.pluginId;
            it.actionId = ref.actionId;
            it.confirm = ref.confirm;
            it.isSwitch = ref.isSwitch;
            it.switchOn = ref.switchOn;
            g.items.push_back(std::move(it));
        }
    }

    g.items.push_back({ L"设置", L"\u2699", ACT_SETTINGS, false, false });
    g.items.push_back({ L"退出", L"\u2715", ACT_EXIT, false, true });
    g.cacheDirty = true;
}

// Agent 精简块: 状态融入开关行文案 (启动=未运行 / 停止=运行中), 运行中才出现「打开」
void PushAgentBlock(const wchar_t* name, WebAgentProc& p, WebAgentProc::Status s) {
    switch (s.phase) {
    case Phase::Running:
        g.items.push_back(MakeAgentItem(ACT_AGENT_OPEN,  p, s,
            std::wstring(L"打开 ") + name, false, false));
        g.items.push_back(MakeAgentItem(ACT_AGENT_STOP,  p, s,
            std::wstring(L"停止 ") + name, false, false));
        break;
    case Phase::Starting:
        g.items.push_back(MakeAgentItem(ACT_AGENT_STOP,  p, s,
            std::wstring(L"停止 (中断启动) - ") + name, false, false));
        break;
    default:
        g.items.push_back(MakeAgentItem(ACT_AGENT_START, p, s,
            std::wstring(L"启动 ") + name, false, false));
        break;
    }
}

Item MakeAgentItem(ActionKind kind, WebAgentProc& p, WebAgentProc::Status s,
                   const std::wstring& label, bool primary, bool danger) {
    Item it;
    it.kind = kind;
    it.agent = &p;
    it.agentName = (&p == &g_app.dshProc) ? L"DSH" : L"Hermes";
    it.label = label;
    it.primary = primary;
    it.danger = danger;
    // 图标按 agent 实时状态着色: 运行绿 / 掉线橙 / 其余默认
    if (s.phase == Phase::Running) it.stateColor = 1;
    else if (s.phase == Phase::Failed) it.stateColor = 2;
    switch (kind) {
    case ACT_AGENT_START:   it.glyph = L"\u25B6"; break;   // ▶
    case ACT_AGENT_STOP:    it.glyph = L"\u25A0"; break;   // ■
    case ACT_AGENT_RESTART: it.glyph = L"\u21BB"; break;   // ↻
    case ACT_AGENT_OPEN:    it.glyph = L"\u2197"; break;   // ↗
    default: break;
    }
    return it;
}

// ---- 渲染 (整窗位图: 阴影 + 圆角面板 + 菜单项) ----
void RenderCache(HWND h) {
    if (!g.cacheDirty && g.cacheBmp) return;
    g.W   = Scale(h, 224);
    g.IH  = Scale(h, 38);
    g.PAD = Scale(h, 8);
    g.SH  = Scale(h, 14);
    // 结果行高度重算 (SetResult 设置的是当时的 DPI scale, 这里按当前窗口重算)
    if (!g.resultLine.empty()) {
        int lines = 1;
        for (wchar_t c : g.resultLine) if (c == L'\n') ++lines;
        g.resultH = lines * Scale(h, 16) + Scale(h, 8);
    } else {
        g.resultH = 0;
    }

    int winW = g.W + g.SH * 2;
    int winH = PanelHeight() + g.SH * 2;

    HDC screen = GetDC(h);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = winW;
    bmi.bmiHeader.biHeight = -winH;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    if (g.cacheBmp) DeleteObject(g.cacheBmp);
    g.cacheBmp = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(h, screen);
    if (!g.cacheBmp) { DeleteDC(mem); return; }
    HGDIOBJ old = SelectObject(mem, g.cacheBmp);
    g.cacheSize = { winW, winH };

    Gdiplus::Graphics gr(mem);
    gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    gr.Clear(Gdiplus::Color(0, 0, 0, 0));

    int px = g.SH, py = g.SH, pw = g.W, ph = PanelHeight();
    Gdiplus::REAL r = (Gdiplus::REAL)Scale(h, 8);

    // 投影: 4 层逐渐加深的圆角描边近似柔影
    for (int i = 4; i >= 1; --i) {
        Gdiplus::GraphicsPath sh;
        sh.AddArc((Gdiplus::REAL)(px - i), (Gdiplus::REAL)(py + i * 2), r * 2, r * 2, 180, 90);
        sh.AddArc((Gdiplus::REAL)(px + pw - r * 2 - i), (Gdiplus::REAL)(py + i * 2), r * 2, r * 2, 270, 90);
        sh.AddArc((Gdiplus::REAL)(px + pw - r * 2 - i), (Gdiplus::REAL)(py + ph - r * 2 + i * 2), r * 2, r * 2, 0, 90);
        sh.AddArc((Gdiplus::REAL)(px - i), (Gdiplus::REAL)(py + ph - r * 2 + i * 2), r * 2, r * 2, 90, 90);
        sh.CloseFigure();
        Gdiplus::Pen p(Gdiplus::Color((BYTE)(10 + (5 - i) * 8), 0, 0, 0), 2.0f);
        gr.DrawPath(&p, &sh);
    }

    // 面板: #1E1E1E 约 95% 不透明 (方案2 4.1: 纯色优先, 不做 Acrylic)
    Gdiplus::GraphicsPath panel;
    panel.AddArc((Gdiplus::REAL)px, (Gdiplus::REAL)py, r * 2, r * 2, 180, 90);
    panel.AddArc((Gdiplus::REAL)(px + pw - r * 2), (Gdiplus::REAL)py, r * 2, r * 2, 270, 90);
    panel.AddArc((Gdiplus::REAL)(px + pw - r * 2), (Gdiplus::REAL)(py + ph - r * 2), r * 2, r * 2, 0, 90);
    panel.AddArc((Gdiplus::REAL)px, (Gdiplus::REAL)(py + ph - r * 2), r * 2, r * 2, 90, 90);
    panel.CloseFigure();
    Gdiplus::SolidBrush panelBrush(Gdiplus::Color(0xF3, 0x1E, 0x1E, 0x1E));
    gr.FillPath(&panelBrush, &panel);
    Gdiplus::Pen edge(Gdiplus::Color(0x50, 0x35, 0xD6, 0xF0), 1.0f);
    gr.DrawPath(&edge, &panel);

    // 菜单项
    Gdiplus::FontFamily fam(L"Segoe UI Symbol");
    for (int i = 0; i < ItemCount(); ++i) {
        const Item& it = g.items[i];
        int iy = py + g.PAD + i * g.IH;
        bool group = (it.kind == ACT_PLUGIN_GROUP);
        bool hot = ((i == g.hover || i == g.sel) && it.interactive());

        if (hot) {
            Gdiplus::GraphicsPath hl;
            Gdiplus::REAL hr = (Gdiplus::REAL)Scale(h, 6);
            int hx = px + Scale(h, 5), hw = pw - Scale(h, 10);
            hl.AddArc((Gdiplus::REAL)hx, (Gdiplus::REAL)iy, hr * 2, hr * 2, 180, 90);
            hl.AddArc((Gdiplus::REAL)(hx + hw - hr * 2), (Gdiplus::REAL)iy, hr * 2, hr * 2, 270, 90);
            hl.AddArc((Gdiplus::REAL)(hx + hw - hr * 2), (Gdiplus::REAL)(iy + g.IH - hr * 2), hr * 2, hr * 2, 0, 90);
            hl.AddArc((Gdiplus::REAL)hx, (Gdiplus::REAL)(iy + g.IH - hr * 2), hr * 2, hr * 2, 90, 90);
            hl.CloseFigure();
            Gdiplus::SolidBrush hb(Gdiplus::Color(0x46, 0x36, 0x44, 0x30));
            gr.FillPath(&hb, &hl);
        }
        if (it.primary) {
            // 主项: accent 竖条 (视觉权重最高, 方案2 4.2)
            Gdiplus::SolidBrush bar(Gdiplus::Color(0xE0, 0x35, 0xD6, 0xF0));
            Gdiplus::Rect b(px + Scale(h, 7), iy + Scale(h, 9), max(2, Scale(h, 3)), g.IH - Scale(h, 18));
            gr.FillRectangle(&bar, b);
        }

        // 图标 (单色符号字形; switch 行画状态环 ◉/○, 分组头无图标)
        // Agent 项按实时状态着色: 运行绿 / 掉线橙
        static const std::wstring kSwOn(L"\u25C9"), kSwOff(L"\u25CB");
        Gdiplus::SolidBrush defIb(Gdiplus::Color(0xE8, it.danger ? 0xE5 : 0x8F, it.danger ? 0x77 : 0xA8, it.danger ? 0x6B : 0xC8));
        Gdiplus::SolidBrush onIb(Gdiplus::Color(0xE8, 0x3F, 0xB9, 0x6C));
        Gdiplus::SolidBrush offIb(Gdiplus::Color(0xE8, 0x8B, 0x93, 0xA7));
        Gdiplus::SolidBrush warnIb(Gdiplus::Color(0xE8, 0xE5, 0xA1, 0x3F));
        const std::wstring* glyph = &it.glyph;
        Gdiplus::SolidBrush* ib = &defIb;
        if (it.stateColor == 1) ib = &onIb;
        else if (it.stateColor == 2) ib = &warnIb;
        if (it.kind == ACT_PLUGIN_SWITCH) {
            glyph = it.switchOn ? &kSwOn : &kSwOff;
            ib = it.switchOn ? &onIb : &offIb;
        }
        if (!glyph->empty()) {
            Gdiplus::Font iconFont(&fam, (Gdiplus::REAL)Scale(h, 15), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            Gdiplus::PointF iconPt((Gdiplus::REAL)(px + Scale(h, 18)),
                                   (Gdiplus::REAL)(iy + g.IH / 2 - Scale(h, 8)));
            gr.DrawString(glyph->c_str(), -1, &iconFont, iconPt, ib);
        }

        // 文字 (分组头: 缩小 + 暗色, 突出下层的动作项)
        Gdiplus::Font textFont(&fam, (Gdiplus::REAL)Scale(h, group ? 12 : 14),
                               it.primary ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular,
                               Gdiplus::UnitPixel);
        Gdiplus::StringFormat sf;
        sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        Gdiplus::SolidBrush tb(group ? Gdiplus::Color(0xB4, 0x6E, 0x7D, 0x91)
                                     : Gdiplus::Color(0xF2, 0xE8, 0xEA, 0xF0));
        Gdiplus::RectF textRect((Gdiplus::REAL)(px + Scale(h, group ? 18 : 46)), (Gdiplus::REAL)iy,
                                (Gdiplus::REAL)(pw - Scale(h, group ? 28 : 96)), (Gdiplus::REAL)g.IH);
        gr.DrawString(it.label.c_str(), -1, &textFont, textRect, &sf, &tb);

        // switch 行: 右侧状态文字
        if (it.kind == ACT_PLUGIN_SWITCH) {
            Gdiplus::Font stFont(&fam, (Gdiplus::REAL)Scale(h, 11), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            Gdiplus::SolidBrush stb(it.switchOn ? Gdiplus::Color(0xE8, 0x3F, 0xB9, 0x6C)
                                                : Gdiplus::Color(0xE8, 0x8B, 0x93, 0xA7));
            Gdiplus::RectF stRect((Gdiplus::REAL)(px + pw - Scale(h, 52)), (Gdiplus::REAL)iy,
                                  (Gdiplus::REAL)Scale(h, 44), (Gdiplus::REAL)g.IH);
            sf.SetAlignment(Gdiplus::StringAlignmentFar);
            gr.DrawString(it.switchOn ? L"ON" : L"OFF", -1, &stFont, stRect, &sf, &stb);
            sf.SetAlignment(Gdiplus::StringAlignmentNear);
        }
    }

    // 底部结果行 (插件动作执行中/完成反馈; 分隔线 + ≤4 行文本)
    if (!g.resultLine.empty() && g.resultH > 0) {
        int ry = py + g.PAD + ItemCount() * g.IH;
        Gdiplus::Pen sep(Gdiplus::Color(0x30, 0x8B, 0x93, 0xA7), 1.0f);
        gr.DrawLine(&sep, (Gdiplus::REAL)(px + Scale(h, 10)), (Gdiplus::REAL)ry,
                    (Gdiplus::REAL)(px + pw - Scale(h, 10)), (Gdiplus::REAL)ry);
        Gdiplus::Font rfont(&fam, (Gdiplus::REAL)Scale(h, 11), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush rbusy(Gdiplus::Color(0xE0, 0x8B, 0x93, 0xA7));
        Gdiplus::SolidBrush rok(Gdiplus::Color(0xE8, 0x3F, 0xB9, 0x6C));
        Gdiplus::SolidBrush rerr(Gdiplus::Color(0xE8, 0xE5, 0x50, 0x4F));
        Gdiplus::SolidBrush& rb = g.resultBusy ? rbusy : (g.resultOk ? rok : rerr);
        Gdiplus::RectF rRect((Gdiplus::REAL)(px + Scale(h, 14)), (Gdiplus::REAL)(ry + Scale(h, 3)),
                             (Gdiplus::REAL)(pw - Scale(h, 24)), (Gdiplus::REAL)(g.resultH - Scale(h, 6)));
        gr.DrawString(g.resultLine.c_str(), -1, &rfont, rRect, nullptr, &rb);
    }

    SelectObject(mem, old);
    DeleteDC(mem);
    g.cacheDirty = false;
}

void Present(HWND h, BYTE alpha) {
    if (!g.cacheBmp) return;
    HDC screen = GetDC(h);
    HDC mem = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(mem, g.cacheBmp);
    ReleaseDC(h, screen);
    POINT dst{ 0, 0 }, src{ 0, 0 };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA };
    UpdateLayeredWindow(h, nullptr, nullptr, &g.cacheSize, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, old);
    DeleteDC(mem);
}

int HitTest(LPARAM l) {
    int x = GET_X_LPARAM(l) - g.SH, y = GET_Y_LPARAM(l) - g.SH;
    if (x < 0 || x > g.W) return -1;
    int rel = y - g.PAD;
    if (rel < 0) return -1;
    int idx = rel / g.IH;
    return (idx >= 0 && idx < ItemCount()) ? idx : -1;
}

void InstallHooks() {
    if (!g.kbHook) g.kbHook = SetWindowsHookExW(WH_KEYBOARD_LL,
        [](int nCode, WPARAM w, LPARAM l) -> LRESULT {
            if (nCode == HC_ACTION && g.visible && !g.fadingOut &&
                (w == WM_KEYDOWN || w == WM_SYSKEYDOWN)) {
                auto* k = (KBDLLHOOKSTRUCT*)l;
                if (k->vkCode == VK_UP || k->vkCode == VK_DOWN ||
                    k->vkCode == VK_RETURN || k->vkCode == VK_ESCAPE) {
                    PostMessageW(g_app.menu, MSG_KEY_NAV, k->vkCode, 0);
                    return 1;   // 菜单打开期间吃掉方向/回车/Esc, 不泄漏给底层窗口
                }
            }
            return CallNextHookEx(nullptr, nCode, w, l);
        }, nullptr, 0);
    if (!g.msHook) g.msHook = SetWindowsHookExW(WH_MOUSE_LL,
        [](int nCode, WPARAM w, LPARAM l) -> LRESULT {
            if (nCode == HC_ACTION && g.visible && !g.fadingOut &&
                (w == WM_LBUTTONDOWN || w == WM_RBUTTONDOWN || w == WM_MBUTTONDOWN)) {
                auto* m = (MSLLHOOKSTRUCT*)l;
                bool inFab = false, inMenu = false;
                RECT r{};
                if (g_app.fab && GetWindowRect(g_app.fab, &r))
                    inFab = (m->pt.x >= r.left && m->pt.x < r.right && m->pt.y >= r.top && m->pt.y < r.bottom);
                if (g_app.menu && GetWindowRect(g_app.menu, &r))
                    inMenu = (m->pt.x >= r.left && m->pt.x < r.right && m->pt.y >= r.top && m->pt.y < r.bottom);
                if (!inFab && !inMenu) PostMessageW(g_app.menu, MSG_OUTSIDE, 0, 0);
            }
            return CallNextHookEx(nullptr, nCode, w, l);
        }, nullptr, 0);
}

void RemoveHooks() {
    if (g.kbHook) { UnhookWindowsHookEx(g.kbHook); g.kbHook = nullptr; }
    if (g.msHook) { UnhookWindowsHookEx(g.msHook); g.msHook = nullptr; }
}

void Execute(Item& it) {
    switch (it.kind) {
    case ACT_AGENT_START: {
        std::wstring err;
        if (!it.agent->Start(err))
            MessageBoxW(g_app.main, err.c_str(),
                        (std::wstring(L"启动 ") + it.agentName).c_str(),
                        MB_OK | MB_ICONWARNING);
        App_RefreshStatusUI();
        break;
    }
    case ACT_AGENT_STOP:    it.agent->Stop();    App_RefreshStatusUI(); break;
    case ACT_AGENT_RESTART: it.agent->Restart(); App_RefreshStatusUI(); break;
    case ACT_AGENT_OPEN:
        if (it.agent == &g_app.dshProc) {
            MainWnd_Show();                     // 双击球 = 回启动器; 菜单项直达浏览器
            it.agent->OpenBrowser();
        } else {
            MainWnd_OpenHermes();               // Hermes: 有嵌入页则切页, 否则浏览器
        }
        break;
    case ACT_PLUGIN:
    case ACT_PLUGIN_SWITCH: {
        // HSPP v1.3: 菜单保持打开, 底部结果行给中间态 (军规: 耗时操作要有中间态)。
        bool isSw = (it.kind == ACT_PLUGIN_SWITCH);
        if (!isSw && !it.confirm.empty()) {
            if (MessageBoxW(g_app.main, it.confirm.c_str(), L"确认操作",
                            MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
                break;
        }
        SetResult(g_app.menu, L"… 执行中", true, true);
        RefreshMenu(g_app.menu);
        TaskRunner::Instance().Post([pid = it.pluginId, aid = it.actionId, isSw,
                                     swOn = it.switchOn]() {
            std::string payload = "{}";
            if (isSw) {
                bool v = !swOn;
                g_app.plugins.setSwitchState(WideToUtf8(pid), WideToUtf8(aid), v);
                payload = std::string("{\"value\":") + (v ? "true" : "false") + "}";
            }
            auto out = g_app.plugins.executeActionEx(WideToUtf8(pid), WideToUtf8(aid), payload);
            std::wstring msg;
            if (out.rc != 0) {
                msg = L"失败 (rc=" + std::to_wstring(out.rc) + L")";
                if (!out.logTail.empty()) msg += L"\n" + Utf8ToWide(out.logTail);
            } else {
                msg = out.logTail.empty() ? L"完成" : Utf8ToWide(out.logTail);
            }
            // 所有权转移给菜单窗口过程 (MSG_RESULT 里 delete)
            PostMessageW(g_app.menu, MSG_RESULT, (WPARAM)(out.rc == 0),
                         (LPARAM)(new std::wstring(std::move(msg))));
        });
        break;
    }
    }
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        int hit = HitTest(l);
        if (hit >= 0 && !g.items[hit].interactive()) hit = -1;   // 分组头不可悬停
        if (hit != g.hover) { g.hover = hit; g.sel = -1; Present(h, (BYTE)g.alpha); }
        return 0;
    }
    case WM_LBUTTONUP: {
        int hit = HitTest(l);
        if (hit >= 0 && hit < ItemCount()) {
            Item it = g.items[hit];        // 拷贝: Hide 会重建 items
            if (it.kind == ACT_PLUGIN || it.kind == ACT_PLUGIN_SWITCH) {
                // 菜单保持打开: 动作异步执行, 结果显示在底部结果行
                g.hover = -1; g.sel = -1;
                Execute(it);
            } else {
                FabMenu_Hide();
                Execute(it);
            }
        }
        return 0;
    }
    case MSG_KEY_NAV: {
        // 分组头不参与键盘导航
        auto interactiveAt = [](int i) {
            return i >= 0 && i < ItemCount() && g.items[i].interactive();
        };
        switch ((int)w) {
        case VK_ESCAPE: FabMenu_Hide(); return 0;
        case VK_UP: {
            int i = g.sel;
            for (int n = 0; n <= ItemCount(); ++n) {
                i = (i <= 0 ? ItemCount() : i) - 1;
                if (interactiveAt(i)) break;
            }
            g.sel = i; g.hover = i;
            Present(h, (BYTE)g.alpha); return 0;
        }
        case VK_DOWN: {
            int i = g.sel;
            for (int n = 0; n <= ItemCount(); ++n) {
                i = (i + 1) % ItemCount();
                if (interactiveAt(i)) break;
            }
            g.sel = i; g.hover = i;
            Present(h, (BYTE)g.alpha); return 0;
        }
        case VK_RETURN:
            if (interactiveAt(g.sel)) {
                Item it = g.items[g.sel];
                if (it.kind == ACT_PLUGIN || it.kind == ACT_PLUGIN_SWITCH) {
                    g.hover = -1; g.sel = -1;
                    Execute(it);
                } else {
                    FabMenu_Hide();
                    Execute(it);
                }
            }
            return 0;
        }
        return 0;
    }
    case MSG_OUTSIDE:
        FabMenu_Hide();
        return 0;
    case MSG_RESULT: {
        // 插件动作异步结果 (TaskRunner 回投, l 指向 new 的 wstring, 此处接管)
        auto* msg = (std::wstring*)l;
        SetResult(h, (w ? L"✓ " : L"✗ ") + *msg, w != 0, false);
        delete msg;
        KillTimer(h, ID_TIMER_RESULT);
        SetTimer(h, ID_TIMER_RESULT, 4000, nullptr);   // 4s 后清除结果行
        RefreshMenu(h);
        return 0;
    }
    case WM_TIMER:
        // SetTimer(nullptr proc) 产生的是 WM_TIMER(wParam=定时器 ID), 不是 MSG_FADE。
        // 旧实现只有 case MSG_FADE —— 淡入淡出从未被驱动, 菜单一直以 alpha=0 呈现 (存量 bug)。
        if ((int)w == ID_TIMER_RESULT) {
            KillTimer(h, ID_TIMER_RESULT);
            g.resultLine.clear();
            g.resultH = 0;
            RefreshMenu(h);
            return 0;
        }
        if ((int)w == ID_TIMER_FADE) { DoFadeStep(h); return 0; }
        return 0;
    case MSG_FADE:                      // 旧消息名, 兼容保留 (实际驱动在 WM_TIMER)
        DoFadeStep(h);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// 淡入淡出单步 (由 WM_TIMER 驱动): 淡出 80ms 量级 / 淡入 ~120ms (方案2 4.1)
void DoFadeStep(HWND h) {
    if (g.fadingOut) {
        g.alpha -= 64;
        if (g.alpha <= 0) {
            KillTimer(h, ID_TIMER_FADE);
            g.alpha = 0;
            ShowWindow(h, SW_HIDE);
            RemoveHooks();
            g.visible = false;
            FabBtn_Internal_MenuClosed();
        } else {
            Present(h, (BYTE)g.alpha);
        }
    } else {
        g.alpha += 51;
        if (g.alpha >= 255) {
            g.alpha = 255;
            KillTimer(h, ID_TIMER_FADE);
        }
        Present(h, (BYTE)g.alpha);
    }
}

}  // namespace

void FabMenu_Create() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = g_app.inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    g_app.menu = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE,
        kClass, L"", WS_POPUP,
        0, 0, 10, 10, nullptr, nullptr, g_app.inst, nullptr);
}

void FabMenu_ShowAtFab() {
    if (!g_app.menu || !g_app.fab) return;
    if (g.visible && !g.fadingOut) return;

    BuildItems();
    RenderCache(g_app.menu);

    RECT fr{};
    GetWindowRect(g_app.fab, &fr);
    HMONITOR mon = MonitorFromWindow(g_app.fab, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);

    int winW = g.W + g.SH * 2, winH = PanelHeight() + g.SH * 2;
    int panelX = (fr.left + fr.right) / 2 - g.W / 2;          // 面板水平居中于按钮
    int panelY = fr.top - Scale(g_app.fab, 8) - PanelHeight(); // 上方 8px
    if (panelY < mi.rcWork.top) panelY = fr.bottom + Scale(g_app.fab, 8);  // 超上沿 -> 下方
    if (panelX + g.W > mi.rcWork.right) panelX = mi.rcWork.right - g.W;
    if (panelX < mi.rcWork.left) panelX = mi.rcWork.left;

    SetWindowPos(g_app.menu, HWND_TOPMOST,
                 panelX - g.SH, panelY - g.SH, winW, winH,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);

    g.hover = -1; g.sel = -1;
    g.visible = true;
    g.fadingOut = false;
    g.alpha = 0;
    InstallHooks();
    Present(g_app.menu, 0);
    SetTimer(g_app.menu, ID_TIMER_FADE, 15, nullptr);
}

void FabMenu_Hide() {
    if (!g_app.menu || !g.visible) return;
    if (g.fadingOut) return;
    g.fadingOut = true;
    g.hover = -1; g.sel = -1;
    // 淡出期间保持可见, MSG_FADE 完成后摘钩子并隐藏
    SetTimer(g_app.menu, ID_TIMER_FADE, 15, nullptr);
}

bool FabMenu_IsVisible() { return g.visible; }

// ---- 插件快捷动作收集 (悬浮菜单 + HTML 概览快捷卡片同源) ----
// HSPP v1.3: 只收 quick 声明开启的插件/动作; switch 动作带宿主侧当前状态。
std::vector<FabActionRef> CollectPluginMenuActions() {
    std::vector<FabActionRef> out;
    if (!g_app.coreReady) return out;
    try {
        for (auto& p : g_app.plugins.loadedPlugins()) {
            if (!p.quick) continue;                       // 清单声明不进快捷区
            auto* entries = g_app.plugins.uiEntriesOf(p.id);
            if (!entries) continue;
            for (auto& u : *entries) {
                if (u.actions.empty()) continue;
                auto acts = nlohmann::json::parse(u.actions, nullptr, true, true);
                for (auto& a : acts) {
                    if (a.value("quick", true) == false) continue;   // 动作级快捷区开关
                    FabActionRef r;
                    r.pluginId   = Utf8ToWide(p.id);
                    r.pluginName = Utf8ToWide(p.name);
                    r.actionId   = Utf8ToWide(a.value("id", ""));
                    r.label      = Utf8ToWide(a.value("label", ""));
                    r.confirm    = Utf8ToWide(a.value("confirm", ""));
                    r.isSwitch   = (a.value("type", std::string("button")) == "switch");
                    r.switchDefault = a.value("default", false);
                    r.switchOn   = r.isSwitch
                        ? g_app.plugins.switchState(p.id, a.value("id", ""), r.switchDefault)
                        : false;
                    if (!r.actionId.empty() && !r.label.empty())
                        out.push_back(std::move(r));
                }
            }
        }
    } catch (...) {}
    return out;
}

}  // namespace hs
