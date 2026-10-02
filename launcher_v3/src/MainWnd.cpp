// MainWnd.cpp —— 主面板: 顶部页签(启动器/Hermes) + 内容区 + 状态栏
//
// ADR-007: WebView2 可用时内容区由 HTML 启动器页/Hermes 页覆盖 (壳只画页签条+状态栏);
// 探测失败回退原生面板 —— 核心功能保留档:
//   概览(生命周期 CTA) 构建★ 更新★ 体检引导 Provider引导 模型引导 插件 设置 日志引导 关于
// 自动切换时序: 启动成功 -> 自动切 Hermes 页签; 停止/掉线 -> 自动回启动器页签。
#include "Ui.h"
#include "WebViews.h"
#include "Brand.h"
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <atomic>
#include "TaskRunner.h"
#include "core/DshRuntime.h"
#include "platform/HttpClient.h"

namespace hs {
namespace {

void ApplyChildControls();   // 前置声明 (相互引用)
void LayoutChildControls(HWND h);

// 关闭窗口记录位置大小 (军规: 用 WINDOWPLACEMENT 的还原矩形, 最大化不污染尺寸)
void SaveWindowBounds() {
    if (!g_app.main) return;
    WINDOWPLACEMENT wp{ sizeof(wp) };
    if (!GetWindowPlacement(g_app.main, &wp)) return;
    if (wp.rcNormalPosition.right <= wp.rcNormalPosition.left) return;
    g_app.cfg.wndX = wp.rcNormalPosition.left;
    g_app.cfg.wndY = wp.rcNormalPosition.top;
    g_app.cfg.wndW = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
    g_app.cfg.wndH = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
    g_app.cfg.wndMax = (wp.showCmd == SW_SHOWMAXIMIZED);
    g_app.cfg.Save(g_app.cfgPath);
}

constexpr wchar_t kClass[] = L"HermesV3MainWnd";
constexpr int ID_TIMER_POLL = 10;

// ---- 命令 (自绘控件) ----
enum {
    CMD_NONE = 0,
    CMD_TAB_LAUNCHER = 90, CMD_TAB_HERMES, CMD_TAB_DSH,
    CMD_FLOAT_HERMES = 93, CMD_FLOAT_DSH,       // 页签条: 拆分/合并当前 agent 页
    CMD_WAIT_START = 95, CMD_WAIT_OPEN,
    CMD_DSH_START = 98, CMD_DSH_OPEN,           // 原生 DSH 等待页按钮
    CMD_NAV = 100,          // +page index
    CMD_START = 200, CMD_STOP, CMD_RESTART, CMD_OPEN, CMD_LOGS, CMD_STATUS, CMD_OPEN_ROOT,
    CMD_CB_FAB = 300, CMD_CB_HIDE, CMD_CB_STOPEXIT, CMD_CB_LAUNCHON, CMD_CB_AUTOCHECK,
    CMD_MODE_DEFERRED = 320, CMD_MODE_IMMEDIATE,
    CMD_DCT_MINUS = 330, CMD_DCT_PLUS,
    CMD_RESET_FAB = 340,
    CMD_OPEN_DL = 390,      // 打开下载缓存目录
    CMD_UPD_SEL = 400,      // +index (更新列表行选择)
    CMD_UPD_BKSEL = 450,    // +index (备份行选择)
    CMD_BUP_SAVE = 470,
    CMD_GUIDE_WV = 480,
};

// ---- 原生页 ----
enum NativePage {
    PG_OVERVIEW = 0, PG_BUILD, PG_UPDATE, PG_DIAG, PG_PROV, PG_MODEL,
    PG_PLUGIN, PG_SETTINGS, PG_LOG, PG_ABOUT, PG_COUNT
};
const wchar_t* kNavNames[] = { L"概览", L"构建", L"更新", L"体检", L"Provider", L"模型",
                               L"插件", L"设置", L"日志", L"关于" };

// ---- 原生子控件 (构建/更新/设置页表单) ----
enum {
    IDC_B_TARGET = 201, IDC_B_BROWSE, IDC_B_RESOLVE, IDC_B_USECACHE, IDC_B_START, IDC_B_CANCEL,
    IDC_U_CHANNEL = 211, IDC_U_SCANLOCAL, IDC_U_SCANREMOTE, IDC_U_APPLY,
    IDC_U_BACKUPS, IDC_U_ROLLBACK,
    IDC_S_PORT = 221, IDC_S_CHANNEL, IDC_S_SAVE,
};
HWND g_ctl[32] = {};

struct Ctrl { RECT rc; int cmd; };

struct UpdRow {
    std::string id, name, current, latest, note;
    bool up = false, supported = false, high = false;
};

struct MainState {
    int  tab = 0;                     // 0 启动器 1 Hermes 2 DSH
    int  page = PG_OVERVIEW;
    bool nativeVisible = true;
    bool autoSwitchOnRun = false;     // 本会话主动启动过 -> Running 时自动切 Hermes 页
    bool autoSwitchDshOnRun = false;  // 同上 (DSH)
    hs::DshProc::Status lastDsh{};    // DSH 相位 (1s 定时器更新, 页签圆点/徽标/原生页共用)
    HWND floatHermes = nullptr;       // 非空 = Hermes 页已拆分为独立窗口
    HWND floatDsh = nullptr;          // 非空 = DSH 页已拆分
    bool dshHttpReady = false;        // DSH HTTP 探测通过 (端口监听后 ~5s 服务才应答)
    std::vector<Ctrl> ctrls;
    int  hoverCmd = CMD_NONE;
    bool tracking = false;
    std::wstring status = L"就绪";
    hs::HermesProc::Status last{};
    Phase prevPhase = Phase::Stopped;
    // 构建页 (简版向导)
    std::wstring buildTarget;         // 空 = 默认 (exe 根)
    std::wstring buildRemoteInfo;
    bool buildRemoteOk = false;
    std::string buildRuntimeVer, buildWebUiVer;
    std::string buildRuntimeUrl, buildWebUiUrl;
    std::wstring buildDlDir;
    double buildRuntimeMB = 0, buildWebUiMB = 0;
    int  buildPercent = 0;
    std::wstring buildStage = L"就绪";
    // 更新页
    std::vector<UpdRow> updRows;
    int  updSel = -1;
    int  updPercent = -1;
    std::wstring updStage;
    int  updAvail = -1;               // update.list 里 updateAvailable 计数 (-1=未检测)
    bool updWithRemote = false;       // 最近一次 update.list 是否含远端比对结果
    struct BackupRow { std::wstring component, tag, path; };
    std::vector<BackupRow> backups;
    int  bkSel = -1;
};
MainState g;
std::atomic<bool> g_dshProbing{false};    // DSH HTTP 探测在途 (后台线程, 结果经 WM_DSH_READY 回 UI)
constexpr UINT WM_DSH_READY = WM_APP + 74;

int TABH(HWND h)  { return ui::Scale(h, 44); }
int NAVW(HWND h)  { return ui::Scale(h, 176); }
int NAVH(HWND h)  { return ui::Scale(h, 40); }
int SBH(HWND h)   { return ui::Scale(h, 36); }

RECT ContentRect(HWND h) {
    RECT rc; GetClientRect(h, &rc);
    rc.top += TABH(h);
    rc.bottom -= SBH(h);
    return rc;
}

void AddCtrl(RECT rc, int cmd) { g.ctrls.push_back({ rc, cmd }); }

// ---- 独立窗口 (拆分/合并, ADR-008) ----
// agent 页可拆出为无页签条的顶层窗口, 与主窗同时显示 (两个及以上 Agent 并排)。
// 实现: WebView2 Controller put_ParentWindow 官方重挂; 关闭独立窗 = 合并回页签。
bool HermesDetached() { return g.floatHermes != nullptr; }
bool DshDetached()    { return g.floatDsh != nullptr; }
void DockAgent(int agent);   // 定义在下 (FloatProc WM_CLOSE 用)
void ApplyContentMode();     // 定义在下 (DetachAgent/DockAgent 末尾刷新用)

LRESULT CALLBACK FloatProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    int agent = (int)(INT_PTR)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_SIZE: {
        RECT rc; GetClientRect(h, &rc);
        if (agent == 1) Wv2_SetHermesBounds(rc); else Wv2_SetDshBounds(rc);
        return 0;
    }
    case WM_CLOSE:
        DockAgent(agent);              // 关闭 = 合并回主窗页签
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

HWND CreateFloat(const wchar_t* cls, const wchar_t* title, int agent, HWND* slot) {
    static bool registered[2] = { false, false };
    int idx = agent - 1;
    if (!registered[idx]) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = FloatProc;
        wc.hInstance = g_app.inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = CreateSolidBrush(RGB(0x0E, 0x11, 0x16));
        wc.lpszClassName = cls;
        RegisterClassExW(&wc);
        registered[idx] = true;
    }
    RECT rc{ 60 + idx * 48, 60 + idx * 48, 60 + idx * 48 + 1240, 60 + idx * 48 + 840 };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND h = CreateWindowExW(0, cls, title, WS_OVERLAPPEDWINDOW,
                             rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                             nullptr, nullptr, g_app.inst,
                             (LPVOID)(INT_PTR)agent);
    SetWindowLongPtrW(h, GWLP_USERDATA, (INT_PTR)agent);
    *slot = h;
    return h;
}

std::wstring DshUrlForEmbed() {
    std::wstring url = g_app.dshProc.TokenUrl();
    if (url.empty()) {
        wchar_t b[64];
        swprintf_s(b, L"http://127.0.0.1:%d/", g_app.settings.dsh.webUiPort);
        url = b;
    }
    return url;
}

void DetachAgent(int agent) {
    if (Wv2_GetState() != Wv2State::Available) return;
    if (agent == 1) {
        if (g.floatHermes || !Wv2_HasHermesView()) return;
        HWND h = CreateFloat(L"HS_FloatHermes", L"Hermes Web UI (关闭窗口 = 合并回主窗)", 1, &g.floatHermes);
        if (!Wv2_ReparentHermes(h)) { DestroyWindow(h); g.floatHermes = nullptr; return; }
        RECT rc; GetClientRect(h, &rc);
        Wv2_SetHermesBounds(rc);
        Wv2_NavigateHermes();              // 独立窗挂载即导航 (页签模式的 ShowHermes 不会执行)
        // CreateFloat 不带 WS_VISIBLE: 先重挂+设尺寸再显示, 避免空框闪现
        ShowWindow(h, SW_SHOW);
        SetForegroundWindow(h);
        logSink().Log(LogLevel::Info, L"[wnd] Hermes 页已拆分为独立窗口");
    } else {
        if (g.floatDsh || !Wv2_HasDshView()) return;
        HWND h = CreateFloat(L"HS_FloatDsh", L"DeepSeek Harness Web UI (关闭窗口 = 合并回主窗)", 2, &g.floatDsh);
        if (!Wv2_ReparentDsh(h)) { DestroyWindow(h); g.floatDsh = nullptr; return; }
        RECT rc; GetClientRect(h, &rc);
        Wv2_SetDshBounds(rc);
        Wv2_NavigateDsh(DshUrlForEmbed());
        ShowWindow(h, SW_SHOW);
        SetForegroundWindow(h);
        logSink().Log(LogLevel::Info, L"[wnd] DSH 页已拆分为独立窗口");
    }
    ApplyContentMode();
}

void DockAgent(int agent) {
    // 先重挂回主窗再销毁独立窗 (顺序相反会连带销毁 WebView2 子 HWND)
    if (agent == 1) {
        if (!g.floatHermes) return;
        Wv2_ReparentHermes(g_app.main);
        DestroyWindow(g.floatHermes);
        g.floatHermes = nullptr;
        logSink().Log(LogLevel::Info, L"[wnd] Hermes 独立窗口已合并回主窗");
    } else {
        if (!g.floatDsh) return;
        Wv2_ReparentDsh(g_app.main);
        DestroyWindow(g.floatDsh);
        g.floatDsh = nullptr;
        logSink().Log(LogLevel::Info, L"[wnd] DSH 独立窗口已合并回主窗");
    }
    ApplyContentMode();
}

COLORREF PhaseColor(Phase p) {
    auto& t = ui::theme();
    switch (p) {
    case Phase::Running:  return t.ok;
    case Phase::Starting: return t.accent;
    case Phase::Failed:   return t.warn;
    default:              return t.muted;
    }
}

// ---- WebView2 / 原生三态协调 ----
void ApplyContentMode() {
    bool covered = false;
    if (Wv2_GetState() == Wv2State::Available) {
        if (g.tab == 0) {
            covered = Wv2_ShowShell();
        } else if (g.tab == 1) {
            // 拆分状态下主窗不嵌入, 显示原生提示页 (独立窗自身已由 FloatProc 管尺寸)
            if (!HermesDetached() && g.last.phase == Phase::Running) covered = Wv2_ShowHermes();
        } else if (g.tab == 2) {
            // HTTP 探测通过才切嵌入 (端口监听后 ~5s 服务才真正应答, 切早了是错误页)
            if (!DshDetached() && g.lastDsh.phase == Phase::Running && g.dshHttpReady)
                covered = Wv2_ShowDsh(DshUrlForEmbed());
        }
    }
    if (!covered) Wv2_Hide();
    g.nativeVisible = !covered;
    FabBtn_ApplyVisibility();
    ApplyChildControls();
    LayoutChildControls(g_app.main);
    InvalidateRect(g_app.main, nullptr, FALSE);
}

// ---- 子控件管理 (仅原生面板可见的页面显示) ----
struct CtlDef { int id; const wchar_t* cls; const wchar_t* text; DWORD style; int page; };
const CtlDef kCtls[] = {
    { IDC_B_TARGET,   L"EDIT",    L"",            WS_CHILD | WS_BORDER | ES_AUTOHSCROLL,           PG_BUILD },
    { IDC_B_BROWSE,   L"BUTTON",  L"浏览...",     WS_CHILD | BS_PUSHBUTTON,                        PG_BUILD },
    { IDC_B_RESOLVE,  L"BUTTON",  L"检查官方版本", WS_CHILD | BS_PUSHBUTTON,                        PG_BUILD },
    { IDC_B_USECACHE, L"BUTTON",  L"使用下载缓存", WS_CHILD | BS_CHECKBOX | BS_PUSHLIKE,            PG_BUILD },
    { IDC_B_START,    L"BUTTON",  L"开始构建",    WS_CHILD | BS_PUSHBUTTON,                        PG_BUILD },
    { IDC_B_CANCEL,   L"BUTTON",  L"取消构建",    WS_CHILD | BS_PUSHBUTTON,                        PG_BUILD },
    { IDC_U_CHANNEL,  L"COMBOBOX", L"",           WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,        PG_UPDATE },
    { IDC_U_SCANLOCAL,L"BUTTON",  L"刷新本地",    WS_CHILD | BS_PUSHBUTTON,                        PG_UPDATE },
    { IDC_U_SCANREMOTE,L"BUTTON", L"检查远端",    WS_CHILD | BS_PUSHBUTTON,                        PG_UPDATE },
    { IDC_U_APPLY,    L"BUTTON",  L"更新选中",    WS_CHILD | BS_PUSHBUTTON,                        PG_UPDATE },
    { IDC_U_BACKUPS,  L"BUTTON",  L"列出备份",    WS_CHILD | BS_PUSHBUTTON,                        PG_UPDATE },
    { IDC_U_ROLLBACK, L"BUTTON",  L"回滚选中备份", WS_CHILD | BS_PUSHBUTTON,                       PG_UPDATE },
    { IDC_S_PORT,     L"EDIT",    L"",            WS_CHILD | WS_BORDER | ES_AUTOHSCROLL,           PG_SETTINGS },
    { IDC_S_CHANNEL,  L"COMBOBOX", L"",           WS_CHILD | CBS_DROPDOWNLIST,                     PG_SETTINGS },
    { IDC_S_SAVE,     L"BUTTON",  L"保存设置",    WS_CHILD | BS_PUSHBUTTON,                        PG_SETTINGS },
};

void CreateChildControls(HWND h) {
    HFONT font = ui::MakeFont(13 * (int)ui::DpiOf(h) / 96, false);
    for (auto& d : kCtls) {
        g_ctl[d.id - 201] = CreateWindowExW(WS_EX_CLIENTEDGE, d.cls, d.text, d.style,
                                            0, 0, 10, 10, h, (HMENU)(INT_PTR)d.id,
                                            g_app.inst, nullptr);
        SendMessageW(g_ctl[d.id - 201], WM_SETFONT, (WPARAM)font, TRUE);
        ShowWindow(g_ctl[d.id - 201], SW_HIDE);
    }
    // 渠道下拉填充
    for (int id : { IDC_U_CHANNEL, IDC_S_CHANNEL }) {
        HWND cb = g_ctl[id - 201];
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"stable");
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"beta");
        SendMessageW(cb, CB_SETCURSEL, 0, 0);
    }
    CheckDlgButton(h, IDC_B_USECACHE, BST_CHECKED);
}

void ApplyChildControls() {
    bool show = g.nativeVisible;
    for (auto& d : kCtls)
        ShowWindow(g_ctl[d.id - 201],
                   (show && d.page == g.page) ? SW_SHOW : SW_HIDE);
}

void LayoutChildControls(HWND h) {
    if (!g_app.main) return;
    RECT rc = ContentRect(h);
    int W = rc.right;
    int nw = g.nativeVisible ? NAVW(h) : 0;
    int bh = ui::Scale(h, 26);
    // 与 Paint* 的标签对齐: cx = nw + Scale(8); 标签 x = cx + M*2 (M=Scale(24))
    int cx = nw + ui::Scale(h, 8);
    int formX = cx + ui::Scale(h, 48 + 150);   // 标签 (M*2) + 标签宽
    switch (g.page) {
    case PG_BUILD: {
        int x = cx + ui::Scale(h, 24);
        int y = rc.top + ui::Scale(h, 70);
        MoveWindow(g_ctl[IDC_B_TARGET - 201], x, y, W - x - ui::Scale(h, 320), bh, TRUE);
        MoveWindow(g_ctl[IDC_B_BROWSE - 201], W - ui::Scale(h, 312), y, ui::Scale(h, 98), bh, TRUE);
        MoveWindow(g_ctl[IDC_B_RESOLVE - 201], W - ui::Scale(h, 206), y, ui::Scale(h, 106), bh, TRUE);
        MoveWindow(g_ctl[IDC_B_USECACHE - 201], W - ui::Scale(h, 96), y, ui::Scale(h, 92), bh, TRUE);
        MoveWindow(g_ctl[IDC_B_START - 201], x, rc.bottom - ui::Scale(h, 48), ui::Scale(h, 120), bh, TRUE);
        MoveWindow(g_ctl[IDC_B_CANCEL - 201], x + ui::Scale(h, 130), rc.bottom - ui::Scale(h, 48), ui::Scale(h, 120), bh, TRUE);
        break;
    }
    case PG_UPDATE: {
        int y = rc.top + ui::Scale(h, 70);
        MoveWindow(g_ctl[IDC_U_CHANNEL - 201], cx + ui::Scale(h, 24), y, ui::Scale(h, 130), bh, TRUE);
        MoveWindow(g_ctl[IDC_U_SCANLOCAL - 201], W - ui::Scale(h, 408), y, ui::Scale(h, 122), bh, TRUE);
        MoveWindow(g_ctl[IDC_U_SCANREMOTE - 201], W - ui::Scale(h, 278), y, ui::Scale(h, 122), bh, TRUE);
        MoveWindow(g_ctl[IDC_U_APPLY - 201], W - ui::Scale(h, 148), y, ui::Scale(h, 130), bh, TRUE);
        MoveWindow(g_ctl[IDC_U_BACKUPS - 201], cx + ui::Scale(h, 24), rc.bottom - ui::Scale(h, 48), ui::Scale(h, 120), bh, TRUE);
        MoveWindow(g_ctl[IDC_U_ROLLBACK - 201], cx + ui::Scale(h, 154), rc.bottom - ui::Scale(h, 48), ui::Scale(h, 150), bh, TRUE);
        break;
    }
    case PG_SETTINGS:
        MoveWindow(g_ctl[IDC_S_PORT - 201], formX, rc.top + ui::Scale(h, 8 + 88), ui::Scale(h, 110), bh, TRUE);
        MoveWindow(g_ctl[IDC_S_CHANNEL - 201], formX, rc.top + ui::Scale(h, 8 + 128), ui::Scale(h, 130), bh * 2, TRUE);
        MoveWindow(g_ctl[IDC_S_SAVE - 201], cx + ui::Scale(h, 24), rc.bottom - ui::Scale(h, 58), ui::Scale(h, 130), ui::Scale(h, 32), TRUE);
        break;
    default:
        break;
    }
}

// ---- 自绘控件 ----
void DrawButton(HDC hdc, HWND hwnd, const RECT& rc, const std::wstring& label,
                int cmd, bool accent, bool danger) {
    auto& t = ui::theme();
    bool hot = (g.hoverCmd == cmd);
    int x = rc.left, y = rc.top, w = rc.right - rc.left, h = rc.bottom - rc.top;
    int r = std::max(4, ui::Scale(hwnd, 6));
    COLORREF fill, text;
    if (accent) {
        fill = hot ? RGB(0x6B, 0xE4, 0xF8) : t.accent;
        text = RGB(0x06, 0x18, 0x22);
    } else if (danger) {
        fill = t.panel; text = t.err;
        if (hot) fill = RGB(0x33, 0x1D, 0x1D);
    } else {
        fill = t.panel; text = t.text;
        if (hot) fill = t.panelHi;
    }
    ui::FillRectRounded(hdc, x, y, w, h, r, fill);
    ui::StrokeRectRounded(hdc, x, y, w, h, r,
                           accent ? (hot ? RGB(0x8A, 0xEB, 0xFB) : t.accentDim) : t.border);
    ui::DrawTextIn(hdc, x, y, w, h, label, text, ui::Scale(hwnd, 13), true, true);
}

void DrawCheck(HDC hdc, HWND hwnd, const RECT& rc, int cmd, bool checked,
               const std::wstring& label) {
    auto& t = ui::theme();
    bool hot = (g.hoverCmd == cmd);
    int s = ui::Scale(hwnd, 18);
    int x = rc.left, y = rc.top + (rc.bottom - rc.top - s) / 2;
    ui::StrokeRectRounded(hdc, x, y, s, s, 4, checked ? t.accent : (hot ? t.text : t.border));
    if (checked) ui::FillRectRounded(hdc, x + 3, y + 3, s - 6, s - 6, 3, t.accent);
    ui::DrawTextIn(hdc, rc.left + s + 8, rc.top, rc.right - rc.left - s - 8,
                   rc.bottom - rc.top, label, t.text, ui::Scale(hwnd, 13));
}

void DrawProgress(HDC hdc, HWND hwnd, const RECT& rc, double pct, COLORREF color) {
    auto& t = ui::theme();
    ui::FillRectRounded(hdc, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                        ui::Scale(hwnd, 5), RGB(0x22, 0x27, 0x33));
    if (pct > 0) {
        int w = (int)((rc.right - rc.left) * min(1.0, max(0.0, pct / 100.0)));
        if (w > ui::Scale(hwnd, 10))
            ui::FillRectRounded(hdc, rc.left, rc.top, w, rc.bottom - rc.top,
                                ui::Scale(hwnd, 5), color);
    }
    ui::StrokeRectRounded(hdc, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                          ui::Scale(hwnd, 5), t.border);
}

// ---- 页签条 ----
void PaintTabBar(HDC hdc, HWND hwnd) {
    auto& t = ui::theme();
    RECT rc; GetClientRect(hwnd, &rc);
    int W = rc.right, th = TABH(hwnd);

    ui::FillRectARect(hdc, 0, 0, W, th, RGB(0x11, 0x14, 0x1B));
    ui::FillRectARect(hdc, 0, th - 1, W, 1, t.border);

    struct TabDef { const wchar_t* label; int cmd; int agent; };   // agent: 0=无点 1=Hermes 2=DSH
    TabDef tabs[] = {
        { L"\u2302  启动器", CMD_TAB_LAUNCHER, 0 },
        { L"Hermes",        CMD_TAB_HERMES,   1 },
        { L"DSH",           CMD_TAB_DSH,      2 },
    };
    const int NTAB = 3;
    int tw = ui::Scale(hwnd, 120);
    for (int i = 0; i < NTAB; ++i) {
        int x = ui::Scale(hwnd, 16) + i * (tw + ui::Scale(hwnd, 6));
        bool sel = (g.tab == i), hot = (g.hoverCmd == tabs[i].cmd);
        if (sel) {
            ui::FillRectRounded(hdc, x, ui::Scale(hwnd, 6), tw, th - ui::Scale(hwnd, 10),
                                ui::Scale(hwnd, 8), RGB(0x1C, 0x22, 0x2D));
        } else if (hot) {
            ui::FillRectRounded(hdc, x, ui::Scale(hwnd, 6), tw, th - ui::Scale(hwnd, 10),
                                ui::Scale(hwnd, 8), RGB(0x17, 0x1B, 0x24));
        }
        if (tabs[i].agent > 0) {   // agent 页签: 圆点独立按相位着色, 不随文字色
            Phase dp = tabs[i].agent == 1 ? g.last.phase : g.lastDsh.phase;
            ui::DrawDot(hdc, x + ui::Scale(hwnd, 17), th / 2, ui::Scale(hwnd, 4), PhaseColor(dp));
            std::wstring label = tabs[i].label;
            if ((tabs[i].agent == 1 && HermesDetached()) || (tabs[i].agent == 2 && DshDetached()))
                label += L" \u29C9";   // ⧉ 已拆分标记
            ui::DrawTextIn(hdc, x + ui::Scale(hwnd, 26), 0, tw - ui::Scale(hwnd, 26), th,
                           label, sel ? t.text : t.muted, ui::Scale(hwnd, 13), sel, true);
        } else {
            ui::DrawTextIn(hdc, x, 0, tw, th, tabs[i].label,
                           sel ? t.text : t.muted, ui::Scale(hwnd, 13), sel, true);
        }
        RECT tr{ x, 0, x + tw, th };
        AddCtrl(tr, tabs[i].cmd);
    }

    // 拆分/合并按钮 (仅 agent 页签可用)
    if (g.tab == 1 || g.tab == 2) {
        bool det = (g.tab == 1) ? HermesDetached() : DshDetached();
        int dbw = ui::Scale(hwnd, 88);
        int dx = ui::Scale(hwnd, 16) + NTAB * (tw + ui::Scale(hwnd, 6)) + ui::Scale(hwnd, 6);
        RECT db{ dx, ui::Scale(hwnd, 8), dx + dbw, th - ui::Scale(hwnd, 8) };
        AddCtrl(db, g.tab == 1 ? CMD_FLOAT_HERMES : CMD_FLOAT_DSH);
        DrawButton(hdc, hwnd, db, det ? L"合并" : L"\u29C9 独立窗口",
                   g.tab == 1 ? CMD_FLOAT_HERMES : CMD_FLOAT_DSH, false, false);
    }
    // 右侧不放状态徽标 —— 双 agent 状态在底部状态栏已有 (重复且启动中会截断)
}

// ---- 左侧导航 ----
void PaintNav(HDC hdc, HWND hwnd, int contentY, int contentH) {
    auto& t = ui::theme();
    RECT rc; GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    int nw = NAVW(hwnd), sbh = SBH(hwnd), th = TABH(hwnd);

    ui::FillRectARect(hdc, 0, th, nw, contentH + th, RGB(0x11, 0x14, 0x1B));

    int ls = ui::Scale(hwnd, 40);
    ui::FillRectRounded(hdc, ui::Scale(hwnd, 18), th + ui::Scale(hwnd, 16), ls, ls,
                        ui::Scale(hwnd, 11), RGB(0x14, 0x2A, 0x33));
    ui::StrokeRectRounded(hdc, ui::Scale(hwnd, 18), th + ui::Scale(hwnd, 16), ls, ls,
                          ui::Scale(hwnd, 11), t.accentDim);
    ui::DrawBolt(hdc, ui::Scale(hwnd, 28), th + ui::Scale(hwnd, 24),
                 (int)(ls * 0.46), (int)(ls * 0.52), t.accent);
    ui::DrawTextIn(hdc, ui::Scale(hwnd, 66), th + ui::Scale(hwnd, 18), nw, ui::Scale(hwnd, 20),
                   HS_APP_NAME, t.text, ui::Scale(hwnd, 13), true);
    ui::DrawTextIn(hdc, ui::Scale(hwnd, 66), th + ui::Scale(hwnd, 38), nw, ui::Scale(hwnd, 16),
                   g_app.rootState == RootState::Full ? L"便携包启动器" : L"待构建模式",
                   g_app.rootState == RootState::Full ? t.muted : t.warn, ui::Scale(hwnd, 10));

    int ny = th + ui::Scale(hwnd, 76);
    for (int i = 0; i < PG_COUNT; ++i) {
        RECT item{ 8, ny, nw - 8, ny + NAVH(hwnd) };
        bool sel = (g.page == i), hot = (g.hoverCmd == CMD_NAV + i);
        if (sel) ui::FillRectRounded(hdc, item.left + 6, item.top + 2,
                                     item.right - item.left - 12, item.bottom - item.top - 4,
                                     ui::Scale(hwnd, 8), RGB(0x1C, 0x22, 0x2D));
        else if (hot) ui::FillRectRounded(hdc, item.left + 6, item.top + 2,
                                          item.right - item.left - 12, item.bottom - item.top - 4,
                                          ui::Scale(hwnd, 8), RGB(0x17, 0x1B, 0x24));
        if (sel) ui::FillRectARect(hdc, item.left + 6, item.top + 10, 3, item.bottom - item.top - 20, t.accent);
        // 核心页标星
        std::wstring name = kNavNames[i];
        int starW = 0;
        if (i == PG_BUILD || i == PG_UPDATE) {
            ui::DrawTextIn(hdc, item.right - ui::Scale(hwnd, 24), item.top,
                           ui::Scale(hwnd, 20), item.bottom - item.top, L"★",
                           t.accent, ui::Scale(hwnd, 11), true, true);
            starW = ui::Scale(hwnd, 20);
        }
        ui::DrawTextIn(hdc, item.left + ui::Scale(hwnd, 20), item.top,
                       item.right - item.left - ui::Scale(hwnd, 26) - starW, item.bottom - item.top,
                       name, sel ? t.text : t.muted, ui::Scale(hwnd, 13), sel);
        AddCtrl(item, CMD_NAV + i);
        ny += NAVH(hwnd) + ui::Scale(hwnd, 3);
    }

    auto& st = g.last;
    ui::DrawDot(hdc, ui::Scale(hwnd, 24), H - sbh - ui::Scale(hwnd, 16), ui::Scale(hwnd, 5),
                PhaseColor(st.phase));
    ui::DrawTextIn(hdc, ui::Scale(hwnd, 36), H - sbh - ui::Scale(hwnd, 27),
                   nw - ui::Scale(hwnd, 44), ui::Scale(hwnd, 22),
                   (std::wstring(L"Hermes ") + PhaseText(st.phase)),
                   PhaseColor(st.phase), ui::Scale(hwnd, 11), true);
}

// ---- 概览页 ----
void PaintOverview(HDC hdc, HWND hwnd, int cx, int cy, int cw, int ch) {
    auto& t = ui::theme();
    auto& st = g.last;
    int M = ui::Scale(hwnd, 24);

    // 待构建模式: 生命周期 CTA
    if (g_app.rootState == RootState::PendingBuild || !g_app.coreReady) {
        int cardH = ui::Scale(hwnd, 220);
        ui::FillRectRounded(hdc, cx, cy, cw, cardH, ui::Scale(hwnd, 10), t.panel);
        ui::StrokeRectRounded(hdc, cx, cy, cw, cardH, ui::Scale(hwnd, 10), t.accentDim);
        ui::DrawTextIn(hdc, cx + M, cy + ui::Scale(hwnd, 20), cw - M * 2, ui::Scale(hwnd, 34),
                       L"便携包尚未构建", t.warn, ui::Scale(hwnd, 22), true);
        ui::DrawTextIn(hdc, cx + M, cy + ui::Scale(hwnd, 62), cw - M * 2, ui::Scale(hwnd, 24),
                       L"从 0 构建一套完整、可移动的 Hermes 便携包 (runtime + web-ui, 首建约 670MB 下载)。",
                       t.text, ui::Scale(hwnd, 13));
        ui::DrawTextIn(hdc, cx + M, cy + ui::Scale(hwnd, 90), cw - M * 2, ui::Scale(hwnd, 24),
                       L"构建完成后启动器自动解锁全部功能 (更新/体检/Provider/模型/插件)。",
                       t.muted, ui::Scale(hwnd, 12));
        int bw = ui::Scale(hwnd, 180), bh = ui::Scale(hwnd, 40);
        RECT go{ cx + M, cy + cardH - bh - ui::Scale(hwnd, 20), cx + M + bw, cy + cardH - ui::Scale(hwnd, 20) };
        AddCtrl(go, CMD_NAV + PG_BUILD);
        DrawButton(hdc, hwnd, go, L"前往构建向导 ★", CMD_NAV + PG_BUILD, true, false);
        return;
    }

    int cardH = ui::Scale(hwnd, 196);
    ui::FillRectRounded(hdc, cx, cy, cw, cardH, ui::Scale(hwnd, 10), t.panel);
    ui::StrokeRectRounded(hdc, cx, cy, cw, cardH, ui::Scale(hwnd, 10), t.border);

    ui::DrawTextIn(hdc, cx + M, cy + ui::Scale(hwnd, 16), ui::Scale(hwnd, 200), ui::Scale(hwnd, 22),
                   L"运行状态", t.muted, ui::Scale(hwnd, 12), true);
    ui::DrawDot(hdc, cx + M + ui::Scale(hwnd, 10), cy + ui::Scale(hwnd, 62), ui::Scale(hwnd, 10),
                PhaseColor(st.phase));
    std::wstring stateWord = PhaseText(st.phase);
    if (st.phase == Phase::Starting) stateWord += L"...";
    ui::DrawTextIn(hdc, cx + M + ui::Scale(hwnd, 30), cy + ui::Scale(hwnd, 40),
                   ui::Scale(hwnd, 280), ui::Scale(hwnd, 44),
                   stateWord, PhaseColor(st.phase), ui::Scale(hwnd, 22), true);
    std::wstring detail;
    switch (st.phase) {
    case Phase::Running:
        detail = (Wv2_GetState() == Wv2State::Available)
            ? L"127.0.0.1:" + std::to_wstring(g_app.port) + L" · Hermes 已嵌入「Hermes」页签"
            : L"127.0.0.1:" + std::to_wstring(g_app.port) + L" · 可切「Hermes」页签或用浏览器打开";
        break;
    case Phase::Starting: detail = L"等待端口就绪 (" + std::to_wstring(st.startingSec) + L"s / 45s)"; break;
    case Phase::Failed:   detail = L"端口无监听, 可从悬浮菜单或此处重启"; break;
    default:              detail = L"启动后自动切换到「Hermes」页签";
    }
    ui::DrawTextIn(hdc, cx + M + ui::Scale(hwnd, 230), cy + ui::Scale(hwnd, 52),
                   ui::Scale(hwnd, 420), ui::Scale(hwnd, 24), detail, t.muted, ui::Scale(hwnd, 12));

    struct Metric { const wchar_t* label; std::wstring value; };
    Metric metrics[4] = {
        { L"PID",        st.phase == Phase::Running ? std::to_wstring(st.pid) : L"—" },
        { L"内存",       st.phase == Phase::Running ? FormatMem(st.memMB) : L"—" },
        { L"运行时长",   st.phase == Phase::Running ? FormatUptime(st.uptimeSec) : L"—" },
        { L"端口",       std::to_wstring(g_app.port) },
    };
    int mw = (cw - M * 2) / 4;
    for (int i = 0; i < 4; ++i) {
        int mx = cx + M + i * mw;
        ui::DrawTextIn(hdc, mx, cy + ui::Scale(hwnd, 96), mw, ui::Scale(hwnd, 20),
                       metrics[i].label, t.muted, ui::Scale(hwnd, 11));
        ui::DrawTextIn(hdc, mx, cy + ui::Scale(hwnd, 116), mw, ui::Scale(hwnd, 26),
                       metrics[i].value, t.text, ui::Scale(hwnd, 15), true);
    }

    int bh = ui::Scale(hwnd, 34), bw = ui::Scale(hwnd, 112);
    int by = cy + cardH - bh - ui::Scale(hwnd, 18);
    int bx = cx + cw - M - bw;
    RECT b3{ bx, by, bx + bw, by + bh }; AddCtrl(b3, CMD_RESTART);
    DrawButton(hdc, hwnd, b3, L"重启网关", CMD_RESTART, st.phase == Phase::Failed, false);
    bx -= bw + ui::Scale(hwnd, 10);
    RECT b2{ bx, by, bx + bw, by + bh };
    bool running = (st.phase == Phase::Running);
    bool starting = (st.phase == Phase::Starting);
    int cmd2 = running ? CMD_STOP : CMD_START;
    AddCtrl(b2, cmd2);
    DrawButton(hdc, hwnd, b2, running ? L"停止" : L"启动", cmd2, !running && !starting, false);
    bx -= bw + ui::Scale(hwnd, 10);
    RECT b1{ bx, by, bx + bw, by + bh }; AddCtrl(b1, CMD_OPEN);
    DrawButton(hdc, hwnd, b1, L"打开 Hermes", CMD_OPEN, running, false);

    // 快速操作
    int qy = cy + cardH + M, qh = ui::Scale(hwnd, 96);
    ui::FillRectRounded(hdc, cx, qy, cw, qh, ui::Scale(hwnd, 10), t.panel);
    ui::StrokeRectRounded(hdc, cx, qy, cw, qh, ui::Scale(hwnd, 10), t.border);
    ui::DrawTextIn(hdc, cx + M, qy + ui::Scale(hwnd, 14), ui::Scale(hwnd, 200), ui::Scale(hwnd, 22),
                   L"快速操作", t.muted, ui::Scale(hwnd, 12), true);
    bw = ui::Scale(hwnd, 122);
    struct QItem { const wchar_t* label; int cmd; } qitems[] = {
        { L"查看日志", CMD_LOGS }, { L"端口 / 状态", CMD_STATUS },
        { L"打开下载目录", CMD_OPEN_DL }, { L"打开根目录", CMD_OPEN_ROOT },
    };
    int bx2 = cx + M, by2 = qy + ui::Scale(hwnd, 42);
    for (int i = 0; i < 4; ++i) {
        RECT qb{ bx2, by2, bx2 + bw, by2 + bh }; AddCtrl(qb, qitems[i].cmd);
        DrawButton(hdc, hwnd, qb, qitems[i].label, qitems[i].cmd, false, false);
        bx2 += bw + ui::Scale(hwnd, 10);
    }

    // 悬浮按钮提示
    int hy = qy + qh + M, hh = ui::Scale(hwnd, 108);
    ui::FillRectRounded(hdc, cx, hy, cw, hh, ui::Scale(hwnd, 10), t.panel);
    ui::StrokeRectRounded(hdc, cx, hy, cw, hh, ui::Scale(hwnd, 10), t.border);
    ui::DrawTextIn(hdc, cx + M, hy + ui::Scale(hwnd, 14), ui::Scale(hwnd, 240), ui::Scale(hwnd, 22),
                   L"悬浮按钮", t.muted, ui::Scale(hwnd, 12), true);
    ui::DrawTextIn(hdc, cx + M, hy + ui::Scale(hwnd, 40), cw - M * 2, ui::Scale(hwnd, 22),
                   L"启动 Hermes 后自动切页并出现悬浮图标: 单击弹菜单 / 双击回启动器", t.text, ui::Scale(hwnd, 13));
    ui::DrawTextIn(hdc, cx + M, hy + ui::Scale(hwnd, 66), cw - M * 2, ui::Scale(hwnd, 22),
                   L"菜单含 停止/重启/日志/插件动作 · 托盘与热键 Ctrl+Shift+L 永远兜底",
                   t.muted, ui::Scale(hwnd, 12));
}

// ---- 构建页 (简版向导) ----
void PaintBuild(HDC hdc, HWND hwnd, int cx, int cy, int cw, int ch) {
    auto& t = ui::theme();
    int M = ui::Scale(hwnd, 24);

    ui::DrawTextIn(hdc, cx + M, cy + ui::Scale(hwnd, 14), ui::Scale(hwnd, 500), ui::Scale(hwnd, 30),
                   L"一键构建便携包 ★", t.text, ui::Scale(hwnd, 16), true);
    ui::DrawTextIn(hdc, cx + M, cy + ui::Scale(hwnd, 44), cw - M * 2, ui::Scale(hwnd, 20),
                   L"官方 releases 两件套。目标目录支持相对路径 (如 .\\Hermes, 相对便携包根); 手动下载的文件按原文件名放入下载目录即命中缓存。",
                   t.muted, ui::Scale(hwnd, 12));

    // 远端版本卡
    int ry = cy + ui::Scale(hwnd, 106), rh = ui::Scale(hwnd, 128);
    ui::FillRectRounded(hdc, cx + M, ry, cw - M * 2, rh, ui::Scale(hwnd, 10), t.panel);
    ui::StrokeRectRounded(hdc, cx + M, ry, cw - M * 2, rh, ui::Scale(hwnd, 10), t.border);
    if (g.buildRemoteOk) {
        wchar_t line[256];
        swprintf_s(line, L"runtime %hs (%.0f MB)   ·   web-ui %hs (%.0f MB)",
                   g.buildRuntimeVer.c_str(), g.buildRuntimeMB,
                   g.buildWebUiVer.c_str(), g.buildWebUiMB);
        ui::DrawTextIn(hdc, cx + M * 2, ry + ui::Scale(hwnd, 14), cw - M * 3, ui::Scale(hwnd, 26),
                       line, t.ok, ui::Scale(hwnd, 14), true);
        ui::DrawTextIn(hdc, cx + M * 2, ry + ui::Scale(hwnd, 44), cw - M * 3, ui::Scale(hwnd, 22),
                       L"sha256 校验将在下载完成后自动执行", t.muted, ui::Scale(hwnd, 12));
        // 手动下载 (网络不畅时用浏览器下载, 原文件名放入下载目录即命中缓存)
        if (!g.buildRuntimeUrl.empty()) {
            int uy = ry + ui::Scale(hwnd, 66);
            ui::DrawTextIn(hdc, cx + M * 2, uy, cw - M * 3, ui::Scale(hwnd, 18),
                           L"手动下载: 用浏览器保存下列文件 (原文件名) 至下载目录, 构建时自动命中缓存",
                           t.muted, ui::Scale(hwnd, 11));
            ui::DrawTextIn(hdc, cx + M * 2, uy + ui::Scale(hwnd, 18), cw - M * 3, ui::Scale(hwnd, 16),
                           std::wstring(L"1. ") + Utf8ToWide(g.buildRuntimeUrl), t.accentDim, ui::Scale(hwnd, 11));
            ui::DrawTextIn(hdc, cx + M * 2, uy + ui::Scale(hwnd, 34), cw - M * 3, ui::Scale(hwnd, 16),
                           std::wstring(L"2. ") + Utf8ToWide(g.buildWebUiUrl), t.accentDim, ui::Scale(hwnd, 11));
            if (!g.buildDlDir.empty())
                ui::DrawTextIn(hdc, cx + M * 2, uy + ui::Scale(hwnd, 50), cw - M * 3, ui::Scale(hwnd, 16),
                               std::wstring(L"下载目录: ") + g.buildDlDir, t.muted, ui::Scale(hwnd, 11));
        }
    } else {
        ui::DrawTextIn(hdc, cx + M * 2, ry + ui::Scale(hwnd, 14), cw - M * 3, ui::Scale(hwnd, 26),
                       g.buildRemoteInfo.empty() ? L"尚未检查官方版本 (点上方「检查官方版本」)"
                                                 : g.buildRemoteInfo,
                       t.muted, ui::Scale(hwnd, 13));
    }

    // 进度
    int py = ry + rh + M;
    DrawProgress(hdc, hwnd, RECT{ cx + M, py, cx + cw - M, py + ui::Scale(hwnd, 18) },
                 g.buildPercent, t.accent);
    ui::DrawTextIn(hdc, cx + M, py + ui::Scale(hwnd, 26), cw - M * 2, ui::Scale(hwnd, 22),
                   g.buildStage + L"  (" + std::to_wstring(g.buildPercent) + L"%)",
                   t.text, ui::Scale(hwnd, 13));
}

// ---- 更新页 (简版) ----
void PaintUpdate(HDC hdc, HWND hwnd, int cx, int cy, int cw, int ch) {
    auto& t = ui::theme();
    int M = ui::Scale(hwnd, 24);

    ui::DrawTextIn(hdc, cx + M, cy + ui::Scale(hwnd, 14), ui::Scale(hwnd, 500), ui::Scale(hwnd, 30),
                   L"组件更新中心 ★", t.text, ui::Scale(hwnd, 16), true);
    ui::DrawTextIn(hdc, cx + M, cy + ui::Scale(hwnd, 42), cw - M * 2, ui::Scale(hwnd, 20),
                   L"更新前自动停止 Hermes, 更新后自动重扫; 每次更新自动备份, 可一键回滚。",
                   t.muted, ui::Scale(hwnd, 12));

    // 列表头
    int ly = cy + ui::Scale(hwnd, 76);
    int colW1 = ui::Scale(hwnd, 170), colW2 = ui::Scale(hwnd, 150), colW3 = ui::Scale(hwnd, 150);
    int rowH = ui::Scale(hwnd, 34);
    auto drawHead = [&](const wchar_t* s, int x, int w) {
        ui::DrawTextIn(hdc, x, ly, w, ui::Scale(hwnd, 22), s, t.muted, ui::Scale(hwnd, 12), true);
    };
    drawHead(L"组件", cx + M, colW1);
    drawHead(L"当前版本", cx + M + colW1, colW2);
    drawHead(L"最新版本", cx + M + colW1 + colW2, colW3);
    ly += ui::Scale(hwnd, 26);

    for (size_t i = 0; i < g.updRows.size(); ++i) {
        auto& r = g.updRows[i];
        bool sel = ((int)i == g.updSel);
        int ry2 = ly + (int)i * rowH;
        if (ry2 + rowH > cy + ch - ui::Scale(hwnd, 110)) break;
        if (sel) ui::FillRectRounded(hdc, cx + M, ry2, cw - M * 2, rowH - 4, ui::Scale(hwnd, 6), RGB(0x1C, 0x22, 0x2D));
        COLORREF nameC = r.high ? t.warn : t.text;
        ui::DrawTextIn(hdc, cx + M + ui::Scale(hwnd, 10), ry2, colW1 - ui::Scale(hwnd, 14), rowH,
                       Utf8ToWide(r.name) + (r.high ? L" (高风险)" : L""),
                       nameC, ui::Scale(hwnd, 13), sel);
        ui::DrawTextIn(hdc, cx + M + colW1, ry2, colW2, rowH,
                       r.current.empty() ? L"(未探测)" : Utf8ToWide(r.current), t.muted, ui::Scale(hwnd, 13));
        std::wstring lat = r.latest.empty() ? L"-" : Utf8ToWide(r.latest);
        ui::DrawTextIn(hdc, cx + M + colW1 + colW2, ry2, colW3, rowH, lat,
                       r.up ? t.err : t.muted, ui::Scale(hwnd, 13), r.up);
        if (r.up)
            ui::DrawTextIn(hdc, cx + M + colW1 + colW2 + colW3, ry2, ui::Scale(hwnd, 90), rowH,
                           L"[有更新]", t.err, ui::Scale(hwnd, 13), true);
        RECT rowRc{ cx + M, ry2, cx + cw - M, ry2 + rowH - 4 };
        AddCtrl(rowRc, CMD_UPD_SEL + (int)i);
    }
    if (g.updRows.empty()) {
        ui::DrawTextIn(hdc, cx + M, ly + ui::Scale(hwnd, 8), cw - M * 2, ui::Scale(hwnd, 24),
                       L"尚无数据。点「刷新本地」先探测本地版本。", t.muted, ui::Scale(hwnd, 13));
    }

    // 进度条 + 备份
    int py = cy + ch - ui::Scale(hwnd, 96);
    if (g.updPercent >= 0) {
        DrawProgress(hdc, hwnd, RECT{ cx + M, py, cx + cw - M, py + ui::Scale(hwnd, 14) },
                     g.updPercent, t.ok);
        ui::DrawTextIn(hdc, cx + M, py + ui::Scale(hwnd, 18), cw - M * 2, ui::Scale(hwnd, 20),
                       g.updStage, t.text, ui::Scale(hwnd, 12));
    }
    // 备份列表 (行选择)
    if (!g.backups.empty()) {
        int byy = py + (g.updPercent >= 0 ? ui::Scale(hwnd, 46) : 0);
        ui::DrawTextIn(hdc, cx + M, byy - ui::Scale(hwnd, 22), ui::Scale(hwnd, 200),
                       ui::Scale(hwnd, 20), L"备份 (选中后点「回滚选中备份」)", t.muted, ui::Scale(hwnd, 11));
        int shown = 0;
        for (size_t i = 0; i < g.backups.size() && shown < 3; ++i, ++shown) {
            auto& b = g.backups[i];
            int ry3 = byy + shown * ui::Scale(hwnd, 24);
            if (ry3 > cy + ch - ui::Scale(hwnd, 56)) break;
            bool sel = ((int)i == g.bkSel);
            if (sel) ui::FillRectRounded(hdc, cx + M, ry3, cw - M * 2, ui::Scale(hwnd, 22),
                                         ui::Scale(hwnd, 5), RGB(0x1C, 0x22, 0x2D));
            ui::DrawTextIn(hdc, cx + M + ui::Scale(hwnd, 10), ry3, cw - M * 2 - ui::Scale(hwnd, 20),
                           ui::Scale(hwnd, 22),
                           b.component + L"/" + b.tag + L"  —  " + b.path,
                           sel ? t.text : t.muted, ui::Scale(hwnd, 12));
            RECT rowRc{ cx + M, ry3, cx + cw - M, ry3 + ui::Scale(hwnd, 22) };
            AddCtrl(rowRc, CMD_UPD_BKSEL + (int)i);
        }
    }
}

// ---- 引导页 (体检/Provider/模型/日志: 需 WebView2) ----
void PaintGuide(HDC hdc, HWND hwnd, int cx, int cy, int cw, int ch, int page) {
    auto& t = ui::theme();
    int M = ui::Scale(hwnd, 24);
    const wchar_t* titles[] = { L"", L"", L"", L"环境体检", L"Provider 管理", L"模型管理",
                                L"", L"", L"运行日志", L"" };
    int cardH = ui::Scale(hwnd, 170);
    ui::FillRectRounded(hdc, cx, cy, cw, cardH, ui::Scale(hwnd, 10), t.panel);
    ui::StrokeRectRounded(hdc, cx, cy, cw, cardH, ui::Scale(hwnd, 10), t.border);
    ui::DrawTextIn(hdc, cx + M, cy + ui::Scale(hwnd, 20), cw - M * 2, ui::Scale(hwnd, 32),
                   titles[page], t.text, ui::Scale(hwnd, 18), true);
    if (Wv2_GetState() == Wv2State::Probing) {
        ui::DrawTextIn(hdc, cx + M, cy + ui::Scale(hwnd, 62), cw - M * 2, ui::Scale(hwnd, 24),
                       L"正在探测 WebView2 可用性...", t.muted, ui::Scale(hwnd, 13));
        return;
    }
    ui::DrawTextIn(hdc, cx + M, cy + ui::Scale(hwnd, 62), cw - M * 2, ui::Scale(hwnd, 24),
                   L"此功能的完整界面需要 WebView2 (当前处于原生回退模式)。", t.muted, ui::Scale(hwnd, 13));
    ui::DrawTextIn(hdc, cx + M, cy + ui::Scale(hwnd, 92), cw - M * 2, ui::Scale(hwnd, 24),
                   L"修复方法: 重装微软 WebView2 Runtime 后重启启动器, 将自动回到完整体。",
                   t.muted, ui::Scale(hwnd, 12));
    int bw = ui::Scale(hwnd, 150), bh = ui::Scale(hwnd, 34);
    RECT b{ cx + M, cy + cardH - bh - ui::Scale(hwnd, 18), cx + M + bw, cy + cardH - ui::Scale(hwnd, 18) };
    AddCtrl(b, CMD_GUIDE_WV);
    DrawButton(hdc, hwnd, b, L"打开官方下载页", CMD_GUIDE_WV, false, false);
}

// ---- 插件页 ----
void PaintPlugins(HDC hdc, HWND hwnd, int cx, int cy, int cw, int ch) {
    auto& t = ui::theme();
    int M = ui::Scale(hwnd, 24);

    ui::DrawTextIn(hdc, cx + M, cy + ui::Scale(hwnd, 14), ui::Scale(hwnd, 300), ui::Scale(hwnd, 30),
                   L"插件 (plugin/)", t.text, ui::Scale(hwnd, 16), true);
    if (!g_app.coreReady) {
        ui::DrawTextIn(hdc, cx + M, cy + ui::Scale(hwnd, 54), cw - M * 2, ui::Scale(hwnd, 24),
                       L"待构建模式下插件不可用。", t.muted, ui::Scale(hwnd, 13));
        return;
    }
    auto plugins = g_app.plugins.allPlugins();
    int py = cy + ui::Scale(hwnd, 58);
    if (plugins.empty()) {
        ui::DrawTextIn(hdc, cx + M, py, cw - M * 2, ui::Scale(hwnd, 26),
                       L"未发现插件。将插件目录放入 <便携包根>/plugin/ 后重新扫描。", t.muted, ui::Scale(hwnd, 13));
        return;
    }
    for (auto& pi : plugins) {
        int chh = ui::Scale(hwnd, 72);
        if (py + chh > cy + ch) break;
        ui::FillRectRounded(hdc, cx + M, py, cw - M * 2, chh, ui::Scale(hwnd, 8), t.panel);
        ui::StrokeRectRounded(hdc, cx + M, py, cw - M * 2, chh, ui::Scale(hwnd, 8), t.border);
        ui::DrawDot(hdc, cx + M + ui::Scale(hwnd, 18), py + ui::Scale(hwnd, 20), ui::Scale(hwnd, 4),
                    pi.loaded ? t.ok : (pi.enabled ? t.err : t.muted));
        std::wstring name = Utf8ToWide(pi.name);
        std::wstring ver = Utf8ToWide(pi.version);
        ui::DrawTextIn(hdc, cx + M + ui::Scale(hwnd, 32), py + ui::Scale(hwnd, 10),
                       ui::Scale(hwnd, 320), ui::Scale(hwnd, 24),
                       name + L"  v" + ver + (pi.enabled ? L"" : L"  (已停用)"),
                       t.text, ui::Scale(hwnd, 14), true);
        std::wstring desc = Utf8ToWide(pi.description);
        ui::DrawTextIn(hdc, cx + M + ui::Scale(hwnd, 32), py + ui::Scale(hwnd, 38),
                       cw - M * 2 - ui::Scale(hwnd, 260), ui::Scale(hwnd, 22),
                       desc.empty() ? L"(无描述)" : desc, t.muted, ui::Scale(hwnd, 12));
        std::wstring state;
        if (!pi.enabled) state = L"已停用";
        else if (pi.loaded) {
            int actCount = 0;
            if (auto* es = g_app.plugins.uiEntriesOf(pi.id))
                for (auto& u : *es) {
                    try { actCount += (int)nlohmann::json::parse(u.actions.empty() ? "[]" : u.actions).size(); }
                    catch (...) {}
                }
            state = actCount > 0 ? L"已加载 · " + std::to_wstring(actCount) + L" 个菜单动作"
                                 : L"已加载";
        } else state = L"加载失败: " + Utf8ToWide(pi.loadError);
        ui::DrawTextIn(hdc, cx + cw - M - ui::Scale(hwnd, 260), py + ui::Scale(hwnd, 10),
                       ui::Scale(hwnd, 246), ui::Scale(hwnd, 24), state,
                       pi.loaded ? t.accent : t.err, ui::Scale(hwnd, 12), false);
        py += chh + ui::Scale(hwnd, 8);
    }
}

// ---- 设置页 ----
void PaintSettings(HDC hdc, HWND hwnd, int cx, int cy, int cw, int ch) {
    auto& t = ui::theme();
    auto& cfg = g_app.cfg;
    auto& rt = g_app.settings.runtime;
    int M = ui::Scale(hwnd, 24);

    ui::DrawTextIn(hdc, cx + M, cy + ui::Scale(hwnd, 14), ui::Scale(hwnd, 300), ui::Scale(hwnd, 30),
                   L"设置", t.text, ui::Scale(hwnd, 16), true);

    // 运行时组
    int gh = ui::Scale(hwnd, 190);
    ui::FillRectRounded(hdc, cx + M, cy + ui::Scale(hwnd, 52), cw - M * 2, gh, ui::Scale(hwnd, 10), t.panel);
    ui::StrokeRectRounded(hdc, cx + M, cy + ui::Scale(hwnd, 52), cw - M * 2, gh, ui::Scale(hwnd, 10), t.border);
    ui::DrawTextIn(hdc, cx + M * 2, cy + ui::Scale(hwnd, 62), ui::Scale(hwnd, 300), ui::Scale(hwnd, 22),
                   L"运行与更新", t.muted, ui::Scale(hwnd, 12), true);
    ui::DrawTextIn(hdc, cx + M * 2, cy + ui::Scale(hwnd, 92), ui::Scale(hwnd, 140), ui::Scale(hwnd, 24),
                   L"Web UI 端口:", t.text, ui::Scale(hwnd, 13));
    ui::DrawTextIn(hdc, cx + M * 2, cy + ui::Scale(hwnd, 132), ui::Scale(hwnd, 140), ui::Scale(hwnd, 24),
                   L"更新通道:", t.text, ui::Scale(hwnd, 13));
    RECT lc{ cx + M * 2 + ui::Scale(hwnd, 320), cy + ui::Scale(hwnd, 92),
             cx + M * 2 + ui::Scale(hwnd, 660), cy + ui::Scale(hwnd, 116) };
    DrawCheck(hdc, hwnd, lc, CMD_CB_LAUNCHON, rt.launchOnStart, L"启动器启动时同时启动 Hermes");
    RECT ac{ cx + M * 2 + ui::Scale(hwnd, 320), cy + ui::Scale(hwnd, 132),
             cx + M * 2 + ui::Scale(hwnd, 660), cy + ui::Scale(hwnd, 156) };
    DrawCheck(hdc, hwnd, ac, CMD_CB_AUTOCHECK, rt.autoCheckUpdate, L"启动时自动检查更新");

    // 悬浮按钮组
    int fy = cy + ui::Scale(hwnd, 52) + gh + ui::Scale(hwnd, 14);
    int fh = ui::Scale(hwnd, 252);
    ui::FillRectRounded(hdc, cx + M, fy, cw - M * 2, fh, ui::Scale(hwnd, 10), t.panel);
    ui::StrokeRectRounded(hdc, cx + M, fy, cw - M * 2, fh, ui::Scale(hwnd, 10), t.border);
    ui::DrawTextIn(hdc, cx + M * 2, fy + ui::Scale(hwnd, 12), ui::Scale(hwnd, 300), ui::Scale(hwnd, 22),
                   L"悬浮按钮", t.muted, ui::Scale(hwnd, 12), true);

    int ly = fy + ui::Scale(hwnd, 40), lh = ui::Scale(hwnd, 30);
    RECT r1{ cx + M * 2, ly, cx + M * 2 + ui::Scale(hwnd, 420), ly + lh }; AddCtrl(r1, CMD_CB_FAB);
    DrawCheck(hdc, hwnd, r1, CMD_CB_FAB, cfg.enabled, L"启用悬浮按钮 (关闭后仅热键/托盘可用)");
    ly += lh;
    RECT r2{ cx + M * 2, ly, cx + M * 2 + ui::Scale(hwnd, 420), ly + lh }; AddCtrl(r2, CMD_CB_HIDE);
    DrawCheck(hdc, hwnd, r2, CMD_CB_HIDE, cfg.hideWhenMainVisible, L"启动器页签可见时隐藏悬浮按钮");
    ly += lh;
    RECT r3{ cx + M * 2, ly, cx + M * 2 + ui::Scale(hwnd, 420), ly + lh }; AddCtrl(r3, CMD_CB_STOPEXIT);
    DrawCheck(hdc, hwnd, r3, CMD_CB_STOPEXIT, cfg.stopHermesOnExit, L"退出启动器时一并停止 Hermes");

    ly += lh + ui::Scale(hwnd, 8);
    ui::DrawTextIn(hdc, cx + M * 2, ly, ui::Scale(hwnd, 110), lh, L"单击行为:", t.text, ui::Scale(hwnd, 13), false, false);
    int sw = ui::Scale(hwnd, 130);
    RECT m1{ cx + M * 2 + ui::Scale(hwnd, 100), ly, cx + M * 2 + ui::Scale(hwnd, 100) + sw, ly + lh };
    AddCtrl(m1, CMD_MODE_DEFERRED);
    DrawButton(hdc, hwnd, m1, L"延迟判定 (可双击)", CMD_MODE_DEFERRED, !cfg.immediateClick, false);
    RECT m2{ m1.right + ui::Scale(hwnd, 8), ly, m1.right + ui::Scale(hwnd, 8) + sw, ly + lh };
    AddCtrl(m2, CMD_MODE_IMMEDIATE);
    DrawButton(hdc, hwnd, m2, L"立即弹菜单", CMD_MODE_IMMEDIATE, cfg.immediateClick, false);

    ly += lh + ui::Scale(hwnd, 8);
    ui::DrawTextIn(hdc, cx + M * 2, ly, ui::Scale(hwnd, 240), lh,
                   cfg.doubleClickTimeMs > 0
                       ? L"双击判定窗口: " + std::to_wstring(cfg.doubleClickTimeMs) + L" ms"
                       : L"双击判定窗口: 系统默认 (" + std::to_wstring(GetDoubleClickTime()) + L" ms)",
                   t.text, ui::Scale(hwnd, 13), false, false);
    int sb = ui::Scale(hwnd, 30);
    RECT minus{ cx + cw - M - ui::Scale(hwnd, 10) - sb * 2 - ui::Scale(hwnd, 8), ly + ui::Scale(hwnd, 2),
                cx + cw - M - ui::Scale(hwnd, 10) - sb - ui::Scale(hwnd, 8), ly + ui::Scale(hwnd, 2) + sb };
    AddCtrl(minus, CMD_DCT_MINUS);
    DrawButton(hdc, hwnd, minus, L"-", CMD_DCT_MINUS, false, false);
    RECT plus{ minus.right + ui::Scale(hwnd, 8), minus.top, minus.right + ui::Scale(hwnd, 8) + sb, minus.bottom };
    AddCtrl(plus, CMD_DCT_PLUS);
    DrawButton(hdc, hwnd, plus, L"+", CMD_DCT_PLUS, false, false);

    ui::DrawTextIn(hdc, cx + M, fy + fh + ui::Scale(hwnd, 12), cw - M * 2, ui::Scale(hwnd, 22),
                   L"设置写入 studio/settings.json (运行时) 与 data/config/fab.json (悬浮按钮)。",
                   t.muted, ui::Scale(hwnd, 12));
}

// ---- 关于页 ----
void PaintAbout(HDC hdc, HWND hwnd, int cx, int cy, int cw, int ch) {
    auto& t = ui::theme();
    int M = ui::Scale(hwnd, 24);
    int ls = ui::Scale(hwnd, 64);
    ui::FillRectRounded(hdc, cx + M, cy + ui::Scale(hwnd, 28), ls, ls, ui::Scale(hwnd, 16), RGB(0x14, 0x2A, 0x33));
    ui::StrokeRectRounded(hdc, cx + M, cy + ui::Scale(hwnd, 28), ls, ls, ui::Scale(hwnd, 16), t.accentDim);
    ui::DrawBolt(hdc, cx + M + (int)(ls * 0.27), cy + ui::Scale(hwnd, 28) + (int)(ls * 0.24),
                 (int)(ls * 0.46), (int)(ls * 0.52), t.accent);
    std::wstring modeNote;
    switch (Wv2_GetState()) {
    case Wv2State::Available:   modeNote = L"WebView2 嵌入模式 (完整体验)"; break;
    case Wv2State::Probing:     modeNote = L"WebView2 探测中..."; break;
    default:                    modeNote = L"原生回退模式 (核心功能保留)"; break;
    }
    ui::DrawTextIn(hdc, cx + M + ls + ui::Scale(hwnd, 18), cy + ui::Scale(hwnd, 32),
                   ui::Scale(hwnd, 460), ui::Scale(hwnd, 32),
                   std::wstring(HS_APP_NAME) + L" 启动器", t.text, ui::Scale(hwnd, 20), true);
    ui::DrawTextIn(hdc, cx + M + ls + ui::Scale(hwnd, 18), cy + ui::Scale(hwnd, 66),
                   ui::Scale(hwnd, 460), ui::Scale(hwnd, 24),
                   std::wstring(L"v") + HS_VERSION_W + L" · ADR-007 生命周期架构 · " + modeNote,
                   t.muted, ui::Scale(hwnd, 12));

    int iy = cy + ui::Scale(hwnd, 124);
    auto info = {
        std::wstring(L"数据根:   ") + g_app.root,
        std::wstring(L"Web UI:   http://127.0.0.1:") + std::to_wstring(g_app.port),
        std::wstring(L"运行时配置: studio/settings.json"),
        std::wstring(L"兜底入口: 托盘图标 / 全局热键 Ctrl+Shift+L"),
    };
    for (auto& s : info) {
        ui::DrawTextIn(hdc, cx + M, iy, cw - M * 2, ui::Scale(hwnd, 24), s, t.text, ui::Scale(hwnd, 13));
        iy += ui::Scale(hwnd, 28);
    }
}

// ---- Agent 原生等待页 (Hermes/DSH 共用唯一实现, 差异由 spec 提供) ----
struct AgentWaitSpec {
    Phase     phase = Phase::Stopped;
    ULONGLONG startingSec = 0;
    const wchar_t* failedTitle = L"已掉线";
    std::wstring idleTitle;          // 停止态标题 (含"未运行/未安装/待构建"等)
    std::wstring hintRunning, hintStarting, hintFailed, hintIdle;
    bool gate = false;               // true = 不画按钮 (待构建/未安装)
    bool detached = false;
    int cmdStart = 0, cmdOpen = 0, cmdDock = 0;
    const wchar_t* startLabel = L"启动";
    const wchar_t* failedLabel = L"重试启动";
};

void PaintAgentWaitPage(HDC hdc, HWND hwnd, int cx, int cy, int cw, int ch,
                        const AgentWaitSpec& s) {
    auto& t = ui::theme();
    int M = ui::Scale(hwnd, 32);

    ui::FillRectARect(hdc, cx, cy, cw, ch, t.bg);
    int midY = cy + ch / 2 - ui::Scale(hwnd, 90);

    ui::DrawDot(hdc, cx + cw / 2, midY + ui::Scale(hwnd, 16), ui::Scale(hwnd, 12), PhaseColor(s.phase));
    std::wstring title = PhaseText(s.phase);
    if (s.phase == Phase::Starting) title += L"...";
    else if (s.phase == Phase::Failed) title = s.failedTitle;
    else if (s.phase == Phase::Stopped) title = s.idleTitle;
    ui::DrawTextIn(hdc, cx + M, midY + ui::Scale(hwnd, 36), cw - M * 2, ui::Scale(hwnd, 44),
                   title, PhaseColor(s.phase), ui::Scale(hwnd, 24), true, true);

    std::wstring hint = s.phase == Phase::Running    ? s.hintRunning
                      : s.phase == Phase::Starting   ? (s.hintStarting + L" (" + std::to_wstring(s.startingSec) + L"s / 45s)")
                      : s.phase == Phase::Failed     ? s.hintFailed
                      : s.hintIdle;
    ui::DrawTextIn(hdc, cx + M, midY + ui::Scale(hwnd, 84), cw - M * 2, ui::Scale(hwnd, 24),
                   hint, t.muted, ui::Scale(hwnd, 13), true);

    int bh = ui::Scale(hwnd, 36), bw = ui::Scale(hwnd, 150);
    int by = midY + ui::Scale(hwnd, 128);
    if (s.detached) {
        int bx = cx + cw / 2 - bw / 2;
        RECT b{ bx, by, bx + bw, by + bh };
        AddCtrl(b, s.cmdDock);
        DrawButton(hdc, hwnd, b, L"合并回主窗", s.cmdDock, true, false);
        return;
    }
    if (s.gate) return;   // 待构建/未安装: 只剩提示
    int bx = cx + cw / 2 - bw / 2;
    RECT b1{ bx, by, bx + bw, by + bh };
    if (s.phase == Phase::Starting) {
        AddCtrl(b1, CMD_NONE);
        DrawButton(hdc, hwnd, b1, L"等待中...", CMD_NONE, true, false);
    } else if (s.phase == Phase::Running) {
        // 运行中只留一个入口 (曾画两个相同的"用浏览器打开")
        AddCtrl(b1, s.cmdOpen);
        DrawButton(hdc, hwnd, b1, L"用浏览器打开", s.cmdOpen, true, false);
    } else {
        AddCtrl(b1, s.cmdStart);
        DrawButton(hdc, hwnd, b1,
                   s.phase == Phase::Failed ? s.failedLabel : s.startLabel,
                   s.cmdStart, true, false);
    }
}

// DSH 原生等待页 (DSH 页签在未运行/WebView 不可用/已拆分时的回退内容)
// ---- 各 Agent 等待页: 只拼差异, 绘制共用 PaintAgentWaitPage ----
void PaintWaitPage(HDC hdc, HWND hwnd, int cx, int cy, int cw, int ch) {
    auto& st = g.last;
    bool pending = g_app.rootState == RootState::PendingBuild;
    AgentWaitSpec s;
    s.phase = st.phase;
    s.startingSec = st.startingSec;
    s.failedTitle = L"Hermes 已掉线";
    s.idleTitle = pending ? L"便携包尚未构建" : L"Hermes 未运行";
    s.detached = HermesDetached();
    s.cmdStart = CMD_WAIT_START; s.cmdOpen = CMD_WAIT_OPEN; s.cmdDock = CMD_FLOAT_HERMES;
    s.startLabel = L"启动 Hermes"; s.failedLabel = L"重启网关";
    s.gate = st.phase == Phase::Stopped && pending;
    switch (st.phase) {
    case Phase::Running:
        s.hintRunning = HermesDetached()
            ? L"Hermes 正在独立窗口中显示; 关闭该窗口可合并回主窗。"
            : (Wv2_GetState() == Wv2State::Available)
            ? L"网关运行中, 正在载入页面..."
            : L"网关运行中。WebView2 嵌入不可用, 请用系统浏览器打开 Hermes。";
        break;
    case Phase::Starting: s.hintStarting = L"已拉起网关, 等待端口就绪 — 就绪后自动进入页面"; break;
    case Phase::Failed:   s.hintFailed = L"端口无监听。重启网关后自动进入 Hermes 页面。"; break;
    default:
        s.hintIdle = pending ? L"请先到「构建」页完成从0构建。"
                             : L"启动 Hermes 后自动进入页面; 也可用系统浏览器打开。";
        break;
    }
    PaintAgentWaitPage(hdc, hwnd, cx, cy, cw, ch, s);
}

void PaintDshWaitPage(HDC hdc, HWND hwnd, int cx, int cy, int cw, int ch) {
    auto& st = g.lastDsh;
    bool installed = hs::dsh::installed();
    AgentWaitSpec s;
    s.phase = st.phase;
    s.startingSec = st.startingSec;
    s.failedTitle = L"DSH 已掉线";
    s.idleTitle = installed ? L"DSH 未运行" : L"DSH 未安装";
    s.detached = DshDetached();
    s.cmdStart = CMD_DSH_START; s.cmdOpen = CMD_DSH_OPEN; s.cmdDock = CMD_FLOAT_DSH;
    s.startLabel = L"启动 DSH"; s.failedLabel = L"重试启动";
    s.gate = st.phase == Phase::Stopped && !installed;
    switch (st.phase) {
    case Phase::Running:
        s.hintRunning = DshDetached()
            ? L"DSH 页面正在独立窗口中显示; 关闭该窗口可合并回主窗。"
            : L"运行中, 正在载入页面...";
        break;
    case Phase::Starting: s.hintStarting = L"已拉起 dsh web, 等待端口就绪 — 就绪后自动进入页面"; break;
    case Phase::Failed:   s.hintFailed = L"端口无监听, 可重试启动。"; break;
    default:
        s.hintIdle = installed ? L"启动 DSH 后自动进入页面; 也可用系统浏览器打开。"
                               : L"请先到「构建」页一键构建 .\\DSH 便携包。";
        break;
    }
    PaintAgentWaitPage(hdc, hwnd, cx, cy, cw, ch, s);
}

void PaintAll(HDC hdc, HWND hwnd) {
    auto& t = ui::theme();
    RECT rc; GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;

    ui::FillRectARect(hdc, 0, 0, W, H, t.bg);
    g.ctrls.clear();

    PaintTabBar(hdc, hwnd);

    int th = TABH(hwnd), sbh = SBH(hwnd);
    int contentY = th, contentH = H - sbh - th;

    if (g.nativeVisible) {
        if (g.tab == 1) {
            PaintWaitPage(hdc, hwnd, 0, contentY, W, contentH);
        } else if (g.tab == 2) {
            PaintDshWaitPage(hdc, hwnd, 0, contentY, W, contentH);
        } else {
            PaintNav(hdc, hwnd, contentY, contentH);
            int nw = NAVW(hwnd);
            int cx = nw + ui::Scale(hwnd, 8), cy = contentY + ui::Scale(hwnd, 8);
            int cw = W - cx - ui::Scale(hwnd, 16), ch = contentH - ui::Scale(hwnd, 16);
            switch (g.page) {
            case PG_OVERVIEW: PaintOverview(hdc, hwnd, cx, cy, cw, ch); break;
            case PG_BUILD:    PaintBuild(hdc, hwnd, cx, cy, cw, ch); break;
            case PG_UPDATE:   PaintUpdate(hdc, hwnd, cx, cy, cw, ch); break;
            case PG_DIAG:     PaintGuide(hdc, hwnd, cx, cy, cw, ch, PG_DIAG); break;
            case PG_PROV:     PaintGuide(hdc, hwnd, cx, cy, cw, ch, PG_PROV); break;
            case PG_MODEL:    PaintGuide(hdc, hwnd, cx, cy, cw, ch, PG_MODEL); break;
            case PG_PLUGIN:   PaintPlugins(hdc, hwnd, cx, cy, cw, ch); break;
            case PG_SETTINGS: PaintSettings(hdc, hwnd, cx, cy, cw, ch); break;
            case PG_LOG:      PaintGuide(hdc, hwnd, cx, cy, cw, ch, PG_LOG); break;
            case PG_ABOUT:    PaintAbout(hdc, hwnd, cx, cy, cw, ch); break;
            }
        }
    }

    // 状态栏
    ui::FillRectARect(hdc, 0, H - sbh, W, sbh, RGB(0x11, 0x14, 0x1B));
    ui::FillRectARect(hdc, 0, H - sbh, W, 1, t.border);
    auto st = g.last;
    // 状态栏右侧三段: 启动器版本/检测 + Hermes + DSH
    {
        int seg = ui::Scale(hwnd, 176);
        // 更新检测段 (最左): 显示当前启动器版本号 + 检测结论
        int ux = W - 3 * (seg + ui::Scale(hwnd, 8)) - ui::Scale(hwnd, 14);
        std::wstring utxt = std::wstring(L"启动器 v" HS_VERSION_W) + L" · 未检测";
        COLORREF ucol = t.muted;
        if (g.updWithRemote && g.updAvail > 0) {
            utxt = std::wstring(L"启动器 v" HS_VERSION_W) + L"  ★"
                 + std::to_wstring(g.updAvail) + L" 可更新";
            ucol = RGB(0xE5, 0xB5, 0x62);
        } else if (g.updWithRemote && g.updAvail == 0) {
            utxt = std::wstring(L"启动器 v" HS_VERSION_W) + L" · 已是最新";
            ucol = RGB(0x3F, 0xCF, 0x8E);
        }
        ui::DrawTextIn(hdc, ux, H - sbh, seg, sbh, utxt, ucol, ui::Scale(hwnd, 12), true);
        // Hermes/DSH 两段在其右侧 (i=0 Hermes 紧邻更新段, i=1 DSH 最右)
        struct B { const wchar_t* name; Phase p; } bs[] = {
            { L"Hermes", st.phase }, { L"DSH", g.lastDsh.phase },
        };
        for (int i = 0; i < 2; ++i) {
            int sx = W - (2 - i) * (seg + ui::Scale(hwnd, 8)) - ui::Scale(hwnd, 14);
            ui::DrawTextIn(hdc, sx, H - sbh, seg, sbh,
                           (std::wstring(L"● ") + bs[i].name + L" " + PhaseText(bs[i].p)),
                           PhaseColor(bs[i].p), ui::Scale(hwnd, 12), true);
        }
        // 左侧状态文字宽度收到更新段之前, 长文本不再压到右侧分段
        ui::DrawTextIn(hdc, ui::Scale(hwnd, 14), H - sbh,
                       ux - ui::Scale(hwnd, 22), sbh,
                       g.status, t.muted, ui::Scale(hwnd, 12));
    }
}

int FindCmd(int x, int y) {
    for (auto& c : g.ctrls)
        if (x >= c.rc.left && x < c.rc.right && y >= c.rc.top && y < c.rc.bottom)
            return c.cmd;
    return CMD_NONE;
}

void SaveFabAndApply() {
    g_app.cfg.Save(g_app.cfgPath);
    FabBtn_ApplyVisibility();
    FabBtn_UpdateVisual();
}

void RunStartStop(int cmd) {
    switch (cmd) {
    case CMD_START: case CMD_WAIT_START: {
        if (g.last.phase == Phase::Starting) { g.status = L"正在启动中, 请稍候..."; return; }
        if (g.last.phase == Phase::Failed) {
            TaskRunner::Instance().Post([]() { g_app.proc.Restart(); App_RefreshStatusUI(); });
            g.status = L"正在重启网关...";
            return;
        }
        Bridge_HandleJson(R"({"cmd":"proc.start"})");
        g.autoSwitchOnRun = true;
        return;
    }
    case CMD_STOP: case CMD_WAIT_OPEN:
        if (cmd == CMD_STOP) {
            Bridge_HandleJson(R"({"cmd":"proc.stop"})");
        } else {
            if (!g_app.proc.IsUp() && g_app.coreReady)
                Bridge_HandleJson(R"({"cmd":"proc.start"})");
            else
                g_app.proc.OpenBrowser();
        }
        return;
    case CMD_RESTART:
        Bridge_HandleJson(R"({"cmd":"proc.restart"})");
        g.autoSwitchOnRun = true;
        return;
    }
}

// ---- Bridge 事件汇入 (原生页面响应同一事件流) ----
void NativeEventSink(const std::string& jsonUtf8) {
    json e;
    try { e = json::parse(jsonUtf8); } catch (...) { return; }
    std::string type = e.value("type", "");

    if (type == "build.remote") {
        g.buildRemoteOk = e.value("ok", false);
        g.buildRemoteInfo = Utf8ToWide(e.value("message", "解析失败"));
        if (g.buildRemoteOk && e.contains("runtime")) {
            g.buildRuntimeVer = e["runtime"].value("version", "?");
            g.buildWebUiVer = e["webUi"].value("version", "?");
            g.buildRuntimeUrl = e["runtime"].value("url", "");
            g.buildWebUiUrl = e["webUi"].value("url", "");
            g.buildRuntimeMB = e["runtime"].value("size", 0.0) / 1048576.0;
            g.buildWebUiMB = e["webUi"].value("size", 0.0) / 1048576.0;
            g.buildDlDir = Utf8ToWide(e.value("downloadDir", ""));
            g.buildRemoteInfo.clear();
        }
        InvalidateRect(g_app.main, nullptr, FALSE);
    } else if (type == "build.progress") {
        g.buildPercent = e.value("percent", 0);
        g.buildStage = Utf8ToWide(e.value("stage", ""));
        InvalidateRect(g_app.main, nullptr, FALSE);
    } else if (type == "build.done") {
        g.buildPercent = e.value("ok", false) ? 100 : 0;
        g.buildStage = e.value("ok", false) ? L"构建完成"
                     : (e.value("cancelled", false) ? L"已取消" : L"构建失败");
        InvalidateRect(g_app.main, nullptr, FALSE);
    } else if (type == "update.list") {
        g.updRows.clear();
        g.updSel = -1;
        int avail = 0;
        if (e.contains("components"))
            for (auto& c : e["components"]) {
                bool up = c.value("updateAvailable", false);
                bool sup = c.value("supported", false);
                // 徽标只数「可一键更新」的组件: Python 等不支持一键更新的行
                // (updateAvailable 可能为真) 数进去会误报 ★N
                if (up && sup) ++avail;
                g.updRows.push_back({ c.value("id", ""), c.value("name", ""),
                                      c.value("current", ""), c.value("latest", ""),
                                      c.value("note", ""), up, sup,
                                      c.value("highRisk", false) });
            }
        // 检测结果上屏: 窗口标题 v版本 + ★N 角标, 与状态栏/网页 pill 同源。
        // 只认含远端比对的扫描 (withRemote), 本地刷新不覆盖已有结论。
        g.updWithRemote = e.value("withRemote", false);
        if (g.updWithRemote) {
            g.updAvail = avail;
            std::wstring t = std::wstring(HS_APP_NAME) + L" v" HS_VERSION_W;
            if (avail > 0) t += L"  ★" + std::to_wstring(avail);
            if (g_app.main) SetWindowTextW(g_app.main, t.c_str());
        }
        InvalidateRect(g_app.main, nullptr, FALSE);
    } else if (type == "update.progress") {
        g.updPercent = e.value("percent", 0);
        g.updStage = Utf8ToWide(e.value("stage", ""));
        InvalidateRect(g_app.main, nullptr, FALSE);
    } else if (type == "update.backupList") {
        g.backups.clear();
        g.bkSel = -1;
        if (e.contains("backups"))
            for (auto& b : e["backups"])
                g.backups.push_back({ Utf8ToWide(b.value("component", "")),
                                      Utf8ToWide(b.value("tag", "")),
                                      Utf8ToWide(b.value("path", "")) });
        InvalidateRect(g_app.main, nullptr, FALSE);
    } else if (type == "op.result") {
        g.status = Utf8ToWide(e.value("message", ""));
        InvalidateRect(g_app.main, nullptr, FALSE);
    }
}

void RunCmd(int cmd) {
    auto& cfg = g_app.cfg;
    auto& rt = g_app.settings.runtime;
    switch (cmd) {
    case CMD_TAB_LAUNCHER: g.tab = 0; ApplyContentMode(); return;
    case CMD_TAB_HERMES:
        g.tab = 1;
        if (HermesDetached()) {            // 已拆分: 聚焦独立窗口
            ShowWindow(g.floatHermes, SW_RESTORE);
            SetForegroundWindow(g.floatHermes);
        } else if (g.last.phase == Phase::Stopped && g_app.coreReady) {
            Bridge_HandleJson(R"({"cmd":"proc.start"})");
            g.autoSwitchOnRun = true;
        }
        ApplyContentMode();
        return;
    case CMD_TAB_DSH:
        g.tab = 2;
        if (DshDetached()) {
            ShowWindow(g.floatDsh, SW_RESTORE);
            SetForegroundWindow(g.floatDsh);
        } else if (g.lastDsh.phase == Phase::Stopped && hs::dsh::installed()) {
            Bridge_HandleJson(R"({"cmd":"dsh.start"})");
            g.autoSwitchDshOnRun = true;
        }
        ApplyContentMode();
        return;
    case CMD_FLOAT_HERMES: DetachAgent(1); return;
    case CMD_FLOAT_DSH:    DetachAgent(2); return;
    case CMD_DSH_START:
        Bridge_HandleJson(R"({"cmd":"dsh.start"})");
        g.autoSwitchDshOnRun = true;
        return;
    case CMD_DSH_OPEN:
        g_app.dshProc.OpenBrowser();
        return;
    case CMD_WAIT_START:
        RunStartStop(CMD_WAIT_START);
        return;
    case CMD_WAIT_OPEN:
        g_app.proc.OpenBrowser();
        return;

    case CMD_START: case CMD_STOP: case CMD_RESTART:
        RunStartStop(cmd);
        return;
    case CMD_OPEN: MainWnd_OpenHermes(); return;
    case CMD_OPEN_ROOT:
        ShellExecuteW(nullptr, L"open", g_app.exeDirW.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    case CMD_OPEN_DL: {
        std::wstring dl = hs::Paths::downloadDir().wstring();
        CreateDirectoryW(dl.c_str(), nullptr);
        ShellExecuteW(nullptr, L"open", dl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    case CMD_LOGS: MenuAction_OpenLogs(); return;
    case CMD_STATUS: MenuAction_ShowStatus(); return;
    case CMD_GUIDE_WV:
        ShellExecuteW(nullptr, L"open",
                      L"https://developer.microsoft.com/microsoft-edge/webview2/",
                      nullptr, nullptr, SW_SHOWNORMAL);
        return;

    case CMD_NAV + PG_BUILD: case CMD_NAV + PG_UPDATE:
    case CMD_NAV + PG_OVERVIEW: case CMD_NAV + PG_DIAG: case CMD_NAV + PG_PROV:
    case CMD_NAV + PG_MODEL: case CMD_NAV + PG_PLUGIN: case CMD_NAV + PG_SETTINGS:
    case CMD_NAV + PG_LOG: case CMD_NAV + PG_ABOUT:
        g.page = cmd - CMD_NAV;
        ApplyChildControls();
        LayoutChildControls(g_app.main);
        break;

    case CMD_CB_FAB:
        cfg.enabled = !cfg.enabled;
        SaveFabAndApply();
        g.status = cfg.enabled ? L"悬浮按钮已启用" : L"悬浮按钮已隐藏 (热键/托盘仍可用)";
        break;
    case CMD_CB_HIDE:
        cfg.hideWhenMainVisible = !cfg.hideWhenMainVisible;
        SaveFabAndApply();
        break;
    case CMD_CB_STOPEXIT:
        cfg.stopHermesOnExit = !cfg.stopHermesOnExit;
        SaveFabAndApply();
        break;
    case CMD_CB_LAUNCHON:
        rt.launchOnStart = !rt.launchOnStart;
        g_app.SaveSettings();
        break;
    case CMD_CB_AUTOCHECK:
        rt.autoCheckUpdate = !rt.autoCheckUpdate;
        g_app.SaveSettings();
        break;
    case CMD_MODE_DEFERRED:
        cfg.immediateClick = false;
        SaveFabAndApply();
        break;
    case CMD_MODE_IMMEDIATE:
        cfg.immediateClick = true;
        SaveFabAndApply();
        break;
    case CMD_DCT_MINUS:
        if (cfg.doubleClickTimeMs <= 0)        cfg.doubleClickTimeMs = 500;
        else if (cfg.doubleClickTimeMs <= 200) cfg.doubleClickTimeMs = 0;
        else                                   cfg.doubleClickTimeMs -= 100;
        SaveFabAndApply();
        break;
    case CMD_DCT_PLUS:
        if (cfg.doubleClickTimeMs <= 0)        cfg.doubleClickTimeMs = 600;
        else                                   cfg.doubleClickTimeMs = std::min(1200, cfg.doubleClickTimeMs + 100);
        SaveFabAndApply();
        break;
    case CMD_RESET_FAB:
        if (g_app.fab) {
            HMONITOR mon = MonitorFromWindow(g_app.fab, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{ sizeof(mi) };
            GetMonitorInfoW(mon, &mi);
            RECT fr{}; GetWindowRect(g_app.fab, &fr);
            int sz = fr.right - fr.left;
            SetWindowPos(g_app.fab, HWND_TOPMOST,
                         mi.rcWork.right - sz - ui::Scale(g_app.fab, 16),
                         mi.rcWork.bottom - sz - ui::Scale(g_app.fab, 16),
                         0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
            RECT fr2{}; GetWindowRect(g_app.fab, &fr2);
            cfg.x = fr2.left; cfg.y = fr2.top;
        }
        cfg.Save(g_app.cfgPath);
        g.status = L"悬浮按钮已重置到工作区右下角";
        break;
    case CMD_BUP_SAVE: {
        // 设置页保存: 端口/通道从子控件读
        wchar_t buf[32]{};
        GetWindowTextW(g_ctl[IDC_S_PORT - 201], buf, 32);
        int port = _wtoi(buf);
        if (port >= 1024 && port <= 65535) rt.webUiPort = port;
        int ch2 = (int)SendMessageW(g_ctl[IDC_S_CHANNEL - 201], CB_GETCURSEL, 0, 0);
        rt.updateChannel = ch2 == 1 ? "beta" : "stable";
        g_app.SaveSettings();
        if (rt.webUiPort > 0) g_app.port = rt.webUiPort;
        g.status = L"设置已保存";
        break;
    }
    default:
        if (cmd >= CMD_UPD_SEL && cmd < CMD_UPD_SEL + 64) {
            int i = cmd - CMD_UPD_SEL;
            g.updSel = (i < (int)g.updRows.size()) ? i : -1;
        } else if (cmd >= CMD_UPD_BKSEL && cmd < CMD_UPD_BKSEL + 64) {
            int i = cmd - CMD_UPD_BKSEL;
            g.bkSel = (i < (int)g.backups.size()) ? i : -1;
        }
        break;
    }
    InvalidateRect(g_app.main, nullptr, TRUE);
}

// 常量对齐 (CMD_UPD_SEL 行选择上限 64 行)
constexpr int CMD_UPD_SEL_BASE = CMD_UPD_SEL;

// ---------- 关闭询问对话框: 最小化 / 退出 + 记住选择 ----------
// 返回 0=最小化 1=退出 -1=取消; remember 输出勾选状态
struct CloseDlgState {
    HWND w = nullptr, parent = nullptr;
    bool remember = false;
    int  result = -1;
    RECT rcMin{}, rcQuit{}, rcCancel{}, rcChk{}, rcText{};
    int  hover = 0;              // 1=min 2=quit 3=cancel 4=chk
    UINT dpi = 96;
    int  W = 0, H = 0;
};
static CloseDlgState g_cd;

void CdPaint(HWND h) {
    auto& t = ui::theme();
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc; GetClientRect(h, &rc);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ oldb = SelectObject(mem, bmp);
    ui::FillRectARect(mem, 0, 0, rc.right, rc.bottom, t.bg);
    ui::FillRectARect(mem, 0, 0, rc.right, ui::Scale(h, 40), RGB(0x11, 0x14, 0x1B));
    ui::DrawTextIn(mem, ui::Scale(h, 18), 0, rc.right - ui::Scale(h, 36), ui::Scale(h, 40),
                   std::wstring(L"关闭 ") + HS_APP_NAME, t.text, ui::Scale(h, 14), true);
    ui::DrawTextIn(mem, ui::Scale(h, 20), g_cd.rcText.top, rc.right - ui::Scale(h, 40),
                   g_cd.rcText.bottom - g_cd.rcText.top,
                   L"要最小化到托盘 (Hermes 保持运行), 还是退出启动器?", t.text, ui::Scale(h, 13));

    auto btn = [&](RECT rc, const std::wstring& label, int hover, bool accent, bool danger) {
        COLORREF fill = accent ? (hover ? RGB(0x6B, 0xE4, 0xF8) : t.accent)
                      : danger ? (hover ? RGB(0x33, 0x1D, 0x1D) : t.panel)
                               : (hover ? t.panelHi : t.panel);
        COLORREF text = accent ? RGB(0x06, 0x18, 0x22) : danger ? t.err : t.text;
        int r = ui::Scale(h, 6);
        ui::FillRectRounded(mem, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, r, fill);
        ui::StrokeRectRounded(mem, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, r,
                              accent ? (hover ? RGB(0x8A, 0xEB, 0xFB) : t.accentDim) : t.border);
        ui::DrawTextIn(mem, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                       label, text, ui::Scale(h, 13), true, true);
    };
    btn(g_cd.rcMin, L"最小化到托盘", g_cd.hover == 1, true, false);
    btn(g_cd.rcQuit, L"退出启动器", g_cd.hover == 2, false, true);
    btn(g_cd.rcCancel, L"取消", g_cd.hover == 3, false, false);

    // 记住选择 复选框
    bool chkHot = (g_cd.hover == 4);
    int s2 = ui::Scale(h, 17);
    int cx = g_cd.rcChk.left, cy = g_cd.rcChk.top + (g_cd.rcChk.bottom - g_cd.rcChk.top - s2) / 2;
    ui::StrokeRectRounded(mem, cx, cy, s2, s2, 4, g_cd.remember ? t.accent : (chkHot ? t.text : t.border));
    if (g_cd.remember) ui::FillRectRounded(mem, cx + 3, cy + 3, s2 - 6, s2 - 6, 3, t.accent);
    ui::DrawTextIn(mem, cx + s2 + 8, g_cd.rcChk.top, rc.right - cx - s2 - 16,
                   g_cd.rcChk.bottom - g_cd.rcChk.top,
                   L"记住我的选择, 下次不再询问", t.text, ui::Scale(h, 13));

    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldb);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(h, &ps);
}

int CdHitTest(int x, int y) {
    auto in = [](RECT rc, int px, int py) {
        return px >= rc.left && px < rc.right && py >= rc.top && py < rc.bottom;
    };
    POINT p{ x, y };
    if (in(g_cd.rcMin, p.x, p.y)) return 1;
    if (in(g_cd.rcQuit, p.x, p.y)) return 2;
    if (in(g_cd.rcCancel, p.x, p.y)) return 3;
    if (in(g_cd.rcChk, p.x, p.y)) return 4;
    return 0;
}

LRESULT CALLBACK CloseDlgWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: CdPaint(h); return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) { g_cd.result = -1; DestroyWindow(h); }
        return 0;
    case WM_MOUSEMOVE: {
        int hit = CdHitTest(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (hit != g_cd.hover) { g_cd.hover = hit; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_LBUTTONUP: {
        int hit = CdHitTest(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (hit == 4) { g_cd.remember = !g_cd.remember; InvalidateRect(h, nullptr, FALSE); return 0; }
        if (hit == 1 || hit == 2) { g_cd.result = hit - 1; DestroyWindow(h); return 0; }
        if (hit == 3) { g_cd.result = -1; DestroyWindow(h); return 0; }   // 取消: 与 ESC 同路
        return 0;
    }
    case WM_DESTROY: g_cd.w = nullptr; return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int ShowCloseDialog(HWND parent, bool& remember) {
    UINT dpi = ui::DpiOf(parent);
    g_cd = CloseDlgState{};
    g_cd.parent = parent;
    g_cd.dpi = dpi;
    g_cd.W = MulDiv(440, dpi, 96);
    g_cd.H = MulDiv(210, dpi, 96);

    RECT pr; GetWindowRect(parent, &pr);
    int x = pr.left + ((pr.right - pr.left) - g_cd.W) / 2;
    int y = pr.top + ((pr.bottom - pr.top) - g_cd.H) / 3;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = CloseDlgWndProc;
    wc.hInstance = g_app.inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"HermesV3CloseDlg";
    RegisterClassExW(&wc);

    // 布局按客户区算, 外框尺寸用 AdjustWindowRect 补标题栏/边框
    RECT need{ 0, 0, g_cd.W, g_cd.H };
    AdjustWindowRect(&need, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE);
    g_cd.w = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, L"",
                             WS_POPUP | WS_CAPTION | WS_SYSMENU,
                             x, y, need.right - need.left, need.bottom - need.top,
                             parent, nullptr, g_app.inst, nullptr);
    if (!g_cd.w) return -1;

    // 布局 (客户区坐标)
    int M = MulDiv(20, dpi, 96);
    int bh = MulDiv(34, dpi, 96);
    g_cd.rcText = { M, MulDiv(52, dpi, 96), g_cd.W - M, MulDiv(84, dpi, 96) };
    g_cd.rcChk = { M, MulDiv(96, dpi, 96), g_cd.W - M, MulDiv(126, dpi, 96) };
    int bw = MulDiv(122, dpi, 96);
    int by = g_cd.H - bh - MulDiv(18, dpi, 96);
    g_cd.rcMin = { M, by, M + bw, by + bh };
    g_cd.rcQuit = { g_cd.rcMin.right + MulDiv(10, dpi, 96), by,
                    g_cd.rcMin.right + MulDiv(10, dpi, 96) + bw, by + bh };
    g_cd.rcCancel = { g_cd.rcQuit.right + MulDiv(10, dpi, 96), by,
                      g_cd.rcQuit.right + MulDiv(10, dpi, 96) + bw, by + bh };

    EnableWindow(parent, FALSE);
    ShowWindow(g_cd.w, SW_SHOW);
    SetForegroundWindow(g_cd.w);
    SetFocus(g_cd.w);
    MSG m;
    while (g_cd.w && IsWindow(g_cd.w)) {
        if (GetMessageW(&m, nullptr, 0, 0) <= 0) break;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    EnableWindow(parent, TRUE);
    remember = g_cd.remember;
    return g_cd.result;
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:
        SetTimer(h, ID_TIMER_POLL, 1000, nullptr);
        g.last = g_app.proc.Query();
        CreateChildControls(h);
g.buildTarget = g_app.exeDirW + L"\\Hermes";
        SetWindowTextW(g_ctl[IDC_B_TARGET - 201], g.buildTarget.c_str());
        Wv2_Resize(ContentRect(h));
        // 未检测到环境: 启动即落在构建页 (用户要求)
        if (g_app.rootState == RootState::PendingBuild) g.page = PG_BUILD;
        return 0;
    case WM_TIMER:
        if (w == ID_TIMER_POLL) {
            Phase before = g.last.phase;
            g.last = g_app.proc.Query();
            if (before != g.last.phase) {   // 相位变化落日志 (失败必须可归因)
                const wchar_t* names[] = { L"stopped", L"starting", L"running", L"failed" };
                auto nm = [&](Phase p) { return names[p >= Phase::Stopped && p <= Phase::Failed ? (int)p : 0]; };
                logSink().Log(LogLevel::Info,
                    std::wstring(L"[proc] phase: ") + nm(before) + L" -> " + nm(g.last.phase) +
                    L" (pid=" + std::to_wstring(g.last.pid) + L", port=" + std::to_wstring(g_app.port) + L")");
                InvalidateRect(h, nullptr, FALSE);   // 页签圆点/按钮随相位即时刷新
                ApplyContentMode();                  // 关键: 停在页签上启动完成也要切嵌入页
            }

            // 自动切换时序 (ADR-007): 启动成功 -> 切 Hermes 页; 停止/掉线 -> 回启动器页
            if (before != Phase::Running && g.last.phase == Phase::Running) {
                Wv2_NavigateHermes();
                if (g.autoSwitchOnRun && g.tab == 0) { g.tab = 1; ApplyContentMode(); }
            } else if (before == Phase::Running &&
                       (g.last.phase == Phase::Stopped || g.last.phase == Phase::Failed)) {
                if (g.tab == 1) { g.tab = 0; ApplyContentMode(); }
            }
            if (g.last.phase == Phase::Running) g.autoSwitchOnRun = false;

            // DSH 相位跟踪 (ADR-008, 与 Hermes 对称)
            {
                Phase dBefore = g.lastDsh.phase;
                g.lastDsh = g_app.dshProc.Query();
                if (dBefore != g.lastDsh.phase) {
                    const wchar_t* dn[] = { L"stopped", L"starting", L"running", L"failed" };
                    logSink().Log(LogLevel::Info,
                        std::wstring(L"[dsh] phase: ") + dn[(int)dBefore] + L" -> " +
                        dn[(int)g.lastDsh.phase] +
                        L" (pid=" + std::to_wstring(g.lastDsh.pid) +
                        L", port=" + std::to_wstring(g_app.settings.dsh.webUiPort) + L")");
                    InvalidateRect(h, nullptr, FALSE);   // 页签圆点随相位即时刷新
                }
                // 关键修复: 用户停在 DSH 页签上点启动后, Running 到来必须切嵌入页;
                // 旧逻辑只在 autoSwitchDshOnRun 且 tab==0 时才 ApplyContentMode,
                // 停在页签上等启动的场景永远停在原生"运行中"页
                if (dBefore != g.lastDsh.phase) ApplyContentMode();
                // 运行中: HTTP 探测就绪后才允许切嵌入 (端口监听 ~5s 后服务才应答)。
                // 探测在后台线程跑 (2s 超时), 结果经 WM_DSH_READY 回 UI, 每秒重试直到就绪
                if (g.lastDsh.phase == Phase::Running) {
                    if (!g.dshHttpReady && !g_dshProbing.exchange(true)) {
                        int port = g_app.settings.dsh.webUiPort;
                        HWND mainH = g_app.main;
                        TaskRunner::Instance().Post([port, mainH]() {
                            net::HttpOptions opt;
                            opt.timeoutSec = 2;
                            auto resp = net::httpGet("http://127.0.0.1:" + std::to_string(port) + "/", opt);
                            PostMessageW(mainH, WM_DSH_READY, resp.status != 0 ? 1 : 0, 0);
                        });
                    }
                } else {
                    g.dshHttpReady = false;   // 离开运行态: 下次启动重新探测
                }
                // 运行中: token 可能已更新 (重启), 嵌入/独立窗都要重新导航
                if (dBefore != Phase::Running && g.lastDsh.phase == Phase::Running) {
                    if (g.lastDsh.pid && !DshDetached() && g.tab != 2)
                        Wv2_NavigateDsh(DshUrlForEmbed());   // 预导航, 页签切换零等待
                    if (g.autoSwitchDshOnRun) {
                        g.autoSwitchDshOnRun = false;
                        if (DshDetached()) { ShowWindow(g.floatDsh, SW_RESTORE); SetForegroundWindow(g.floatDsh); }
                        else if (g.tab == 0) { g.tab = 2; ApplyContentMode(); }
                    }
                } else if (dBefore == Phase::Running &&
                           (g.lastDsh.phase == Phase::Stopped || g.lastDsh.phase == Phase::Failed)) {
                    if (g.tab == 2 && !DshDetached()) { g.tab = 0; ApplyContentMode(); }
                }
            }

            // proc.state 事件 -> HTML (周期推送)
            {
                nlohmann::json e;
                e["type"] = "proc.state";
                e["phase"] = g.last.phase == Phase::Running ? "running"
                          : g.last.phase == Phase::Starting ? "starting"
                          : g.last.phase == Phase::Failed ? "failed" : "stopped";
                e["pid"] = g.last.pid;
                e["memMB"] = g.last.memMB;
                e["uptimeSec"] = g.last.uptimeSec;
                e["startingSec"] = g.last.startingSec;
                e["port"] = g_app.port;
                e["rootState"] = g_app.rootState == RootState::Full ? "full" : "pendingBuild";
                e["coreReady"] = g_app.coreReady;
                // DSH 子状态 (ADR-008): 与 Hermes 相位独立
                {
                    auto& ds = g.lastDsh;
                    Phase dp = ds.phase;
                    json d;
                    d["phase"] = dp == Phase::Running ? "running"
                              : dp == Phase::Starting ? "starting"
                              : dp == Phase::Failed ? "failed" : "stopped";
                    d["pid"] = ds.pid;
                    d["memMB"] = ds.memMB;
                    d["uptimeSec"] = ds.uptimeSec;
                    d["startingSec"] = ds.startingSec;
                    d["port"] = g_app.settings.dsh.webUiPort;
                    d["installed"] = hs::dsh::installed();
                    d["tokenReady"] = !g_app.dshProc.TokenUrl().empty();
                    d["detached"] = DshDetached();
                    e["dsh"] = d;
                }
                Bridge_PushEvent(e.dump());
            }
            FabBtn_UpdateVisual();
            Tray_UpdateTip();
        }
        return 0;
    case WM_WV2_STATE:
        g.status = (w ? L"WebView2 就绪: 启动器页已嵌入 (完整体验)"
                      : L"WebView2 不可用, 已回退原生面板 (核心功能保留)");
        ApplyContentMode();
        if (w) Bridge_FlushQueuedEvents();   // 冲刷探测期间排队的事件给页面
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        PaintAll(mem, h);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!g.tracking) {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
            TrackMouseEvent(&tme);
            g.tracking = true;
        }
        int cmd = FindCmd(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (cmd != g.hoverCmd) {
            g.hoverCmd = cmd;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g.tracking = false;
        if (g.hoverCmd != CMD_NONE) {
            g.hoverCmd = CMD_NONE;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP: {
        int cmd = FindCmd(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (cmd != CMD_NONE) RunCmd(cmd);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(w);
        switch (id) {
        // 托盘菜单
        case 1: MainWnd_Show(); break;
        case 2: MainWnd_OpenHermes(); break;
        case 3: RunCmd(CMD_START); break;
        case 4: RunCmd(CMD_STOP); break;
        case 6: MenuAction_OpenLogs(); break;
        case 5: MainWnd_RequestQuit(); break;
        // 构建页
        case IDC_B_BROWSE: {
            wchar_t dir[MAX_PATH]{};
            GetWindowTextW(g_ctl[IDC_B_TARGET - 201], dir, MAX_PATH);
            BROWSEINFOW bi{};
            bi.hwndOwner = h;
            bi.lpszTitle = L"选择便携包目标目录";
            bi.ulFlags = BIF_RETURNONLYFSDIRS;
            PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
            if (pidl) {
                if (SHGetPathFromIDListW(pidl, dir))
                    SetWindowTextW(g_ctl[IDC_B_TARGET - 201], dir);
                CoTaskMemFree(pidl);
            }
            break;
        }
        case IDC_B_RESOLVE:
            Bridge_HandleJson(R"({"cmd":"build.resolve"})");
            g.buildRemoteInfo = L"正在解析官方最新版本...";
            InvalidateRect(h, nullptr, TRUE);
            break;
        case IDC_B_START: {
            wchar_t dir[MAX_PATH]{};
            GetWindowTextW(g_ctl[IDC_B_TARGET - 201], dir, MAX_PATH);
            // 支持 .\Hermes 等相对写法 (相对便携包根/exe 所在目录)
            std::wstring t = dir;
            bool isAbs = (t.size() >= 2 && t[1] == L':') || t[0] == L'\\' || t[0] == L'/';
            if (!isAbs && !t.empty()) t = std::wstring(g_app.exeDirW) + L"\\" + t;
            g.buildTarget = t;
            bool useCache = IsDlgButtonChecked(h, IDC_B_USECACHE) == BST_CHECKED;
            nlohmann::json req;
            req["cmd"] = "build.start";
            req["targetDir"] = WideToUtf8(g.buildTarget);
            req["useCache"] = useCache;
            Bridge_HandleJson(req.dump());
            g.buildPercent = 0;
            g.buildStage = L"准备构建...";
            InvalidateRect(h, nullptr, TRUE);
            break;
        }
        case IDC_B_CANCEL:
            Bridge_HandleJson(R"({"cmd":"build.cancel"})");
            break;
        // 更新页
        case IDC_U_SCANLOCAL:
            Bridge_HandleJson(R"({"cmd":"update.scanLocal"})");
            break;
        case IDC_U_SCANREMOTE: {
            int ch2 = (int)SendMessageW(g_ctl[IDC_U_CHANNEL - 201], CB_GETCURSEL, 0, 0);
            g_app.settings.runtime.updateChannel = ch2 == 1 ? "beta" : "stable";
            g_app.SaveSettings();
            nlohmann::json req;
            req["cmd"] = "update.scanRemote";
            req["channel"] = g_app.settings.runtime.updateChannel;
            Bridge_HandleJson(req.dump());
            break;
        }
        case IDC_U_APPLY: {
            if (g.updSel < 0 || g.updSel >= (int)g.updRows.size()) { g.status = L"请先选择组件"; InvalidateRect(h, nullptr, TRUE); break; }
            auto& r = g.updRows[g.updSel];
            if (!r.up) { g.status = L"该组件已是最新"; InvalidateRect(h, nullptr, TRUE); break; }
            nlohmann::json req;
            req["cmd"] = "update.apply";
            req["componentId"] = r.id;
            req["channel"] = g_app.settings.runtime.updateChannel;
            Bridge_HandleJson(req.dump());
            break;
        }
        case IDC_U_BACKUPS:
            Bridge_HandleJson(R"({"cmd":"update.backups"})");
            break;
        case IDC_U_ROLLBACK: {
            if (g.bkSel < 0 || g.bkSel >= (int)g.backups.size()) { g.status = L"请先列出并选择备份"; InvalidateRect(h, nullptr, TRUE); break; }
            nlohmann::json req;
            req["cmd"] = "update.rollback";
            req["path"] = WideToUtf8(g.backups[g.bkSel].path);
            Bridge_HandleJson(req.dump());
            break;
        }
        case IDC_S_SAVE:
            RunCmd(CMD_BUP_SAVE);
            break;
        }
        return 0;
    }
    case WM_SIZE:
        Wv2_Resize(ContentRect(h));
        LayoutChildControls(h);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_CLOSE: {
        SaveWindowBounds();             // 无论哪种关闭, 都先记录位置大小
        if (g_app.cfg.closeAction == 1) {   // 记住的选择: 最小化
            ShowWindow(h, SW_HIDE);
            FabBtn_ApplyVisibility();
            return 0;
        }
        if (g_app.cfg.closeAction == 2) {   // 记住的选择: 退出
            MainWnd_RequestQuit();
            return 0;
        }
        bool remember = false;
        int c = ShowCloseDialog(h, remember);
        if (c == 0) {                        // 最小化到托盘
            if (remember) { g_app.cfg.closeAction = 1; g_app.cfg.Save(g_app.cfgPath); }
            ShowWindow(h, SW_HIDE);
            FabBtn_ApplyVisibility();
        } else if (c == 1) {                 // 退出
            if (remember) { g_app.cfg.closeAction = 2; g_app.cfg.Save(g_app.cfgPath); }
            MainWnd_RequestQuit();
        }
        return 0;
    }
    case WM_DPICHANGED: {
        auto* sug = (RECT*)l;
        SetWindowPos(h, nullptr, sug->left, sug->top,
                     sug->right - sug->left, sug->bottom - sug->top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        Wv2_Resize(ContentRect(h));
        LayoutChildControls(h);
        FabBtn_NotifyDpiChanged();
        return 0;
    }
    case WM_TRAYICON:
        if (LOWORD(l) == WM_LBUTTONUP || LOWORD(l) == WM_LBUTTONDBLCLK) MainWnd_Show();
        else if (LOWORD(l) == WM_RBUTTONUP) Tray_ShowMenu(h);
        return 0;
    case WM_HOTKEY:
        if ((int)w == g_app.hotKeyId) MainWnd_Show();
        return 0;
    case WM_DSH_READY:
        // HTTP 探测结果 (后台线程 → UI): 就绪则立即尝试切嵌入页
        g_dshProbing = false;
        if (w) {
            g.dshHttpReady = true;
            ApplyContentMode();
        }
        return 0;
    case WM_DESTROY:
        // 独立窗口: 随主窗退出 (先清 slot 防止 WM_CLOSE 触发合并逻辑)
        if (g.floatHermes) { HWND f = g.floatHermes; g.floatHermes = nullptr; DestroyWindow(f); }
        if (g.floatDsh)    { HWND f = g.floatDsh;    g.floatDsh = nullptr;    DestroyWindow(f); }
        SaveWindowBounds();
        KillTimer(h, ID_TIMER_POLL);
        PostQuitMessage(0);
        return 0;
    default:
        if (g_app.wmTaskbarRestart && m == g_app.wmTaskbarRestart && g_app.main == h)
            Tray_Add(h);
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

// Bridge_PushEvent -> 原生汇入 (定义在 Bridge.cpp 调用)
void MainWnd_NativeEventSink(const std::string& json) {
    NativeEventSink(json);
}

void MainWnd_Create(int nCmdShow) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = g_app.inst;
    wc.hIcon = (HICON)LoadImageW(g_app.inst, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                 GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    wc.hIconSm = (HICON)LoadImageW(g_app.inst, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    logSink().Log(LogLevel::Info,
        L"[wnd] create: cfg.wndW=" + std::to_wstring(g_app.cfg.wndW) +
        L" wndH=" + std::to_wstring(g_app.cfg.wndH) +
        L" wndX=" + std::to_wstring(g_app.cfg.wndX) +
        L" wndY=" + std::to_wstring(g_app.cfg.wndY));
    UINT dpi = ui::DpiOf(GetDesktopWindow());
    int W = MulDiv(1020, dpi, 96), H = MulDiv(680, dpi, 96);
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    bool restored = false;
    logSink().Log(LogLevel::Info,
        L"[wnd] MainWnd_Create: cfg.wndW=" + std::to_wstring(g_app.cfg.wndW) +
        L" wndH=" + std::to_wstring(g_app.cfg.wndH) +
        L" wndX=" + std::to_wstring(g_app.cfg.wndX) +
        L" wndY=" + std::to_wstring(g_app.cfg.wndY));
    if (g_app.cfg.wndW >= MulDiv(480, dpi, 96) && g_app.cfg.wndH >= MulDiv(320, dpi, 96)) {
        W = g_app.cfg.wndW;
        H = g_app.cfg.wndH;
        if (g_app.cfg.wndX > -10000 && g_app.cfg.wndY > -10000) {
            RECT rc{ g_app.cfg.wndX, g_app.cfg.wndY,
                     g_app.cfg.wndX + W, g_app.cfg.wndY + H };
            if (MonitorFromRect(&rc, MONITOR_DEFAULTTONULL)) {   // 必须落在某显示器内
                x = g_app.cfg.wndX;
                y = g_app.cfg.wndY;
                restored = true;
            }
        }
    }

    g_app.main = CreateWindowExW(
        0, kClass, HS_APP_NAME L" v" HS_VERSION_W,
        WS_OVERLAPPEDWINDOW,
        x, y, W, H,
        nullptr, nullptr, g_app.inst, nullptr);
    ShowWindow(g_app.main, restored && g_app.cfg.wndMax ? SW_SHOWMAXIMIZED : nCmdShow);
    UpdateWindow(g_app.main);
    LayoutChildControls(g_app.main);
}

void MainWnd_Show() {
    if (!g_app.main) return;
    ShowWindow(g_app.main, SW_RESTORE);
    SetForegroundWindow(g_app.main);
    FabBtn_ApplyVisibility();
}

void MainWnd_OpenHermes() {
    if (Wv2_GetState() == Wv2State::Available) {
        MainWnd_Show();
        MainWnd_Navigate(4);        // 4 = Hermes 页签 (未运行则自动启动)
    } else {
        if (!g_app.proc.IsUp() && g_app.coreReady) {
            Bridge_HandleJson(R"({"cmd":"proc.start"})");
            App_RefreshStatusUI();
        }
        g_app.proc.OpenBrowser();
    }
}

int MainWnd_CurrentTab() { return g.tab; }

void MainWnd_Navigate(int target) {
    if (target == 4) {
        g.tab = 1;
        if (g.last.phase == Phase::Stopped && g_app.coreReady) {
            Bridge_HandleJson(R"({"cmd":"proc.start"})");
            g.autoSwitchOnRun = true;
        }
        ApplyContentMode();
    } else {
        g.tab = 0;
        if (target >= 0 && target < PG_COUNT) {
            g.page = target;
            ApplyChildControls();
            LayoutChildControls(g_app.main);
        }
        ApplyContentMode();
    }
    InvalidateRect(g_app.main, nullptr, TRUE);
}

void MainWnd_Refresh() {
    g.last = g_app.proc.Query();
    ApplyContentMode();
}

void MainWnd_Log(const std::wstring& line) {
    g.status = line;
    InvalidateRect(g_app.main, nullptr, FALSE);
}

void MainWnd_RequestQuit() {
    if (!g_app.main) return;
    if (g_app.proc.IsUp() && !g_app.cfg.stopHermesOnExit) {
        int rc = MessageBoxW(g_app.main,
                             L"Hermes 正在运行。\n\n停止 Hermes 并退出?选「否」则保持 Hermes 运行, 仅退出启动器。",
                             (std::wstring(L"退出 ") + HS_APP_NAME).c_str(),
                             MB_YESNOCANCEL | MB_ICONQUESTION);
        if (rc == IDCANCEL) return;
        if (rc == IDYES) g_app.proc.Stop();
    } else if (g_app.cfg.stopHermesOnExit && g_app.proc.IsUp()) {
        g_app.proc.Stop();
    }
    DestroyWindow(g_app.main);
}

void MenuAction_OpenLogs() {
    std::wstring dir = g_app.LogsDirW();
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void MenuAction_ShowStatus() {
    auto st = g_app.proc.Query();
    std::wstring msg;
    switch (st.phase) {
    case Phase::Running:
        msg = L"Hermes 正在运行\n\n端口: 127.0.0.1:" + std::to_wstring(g_app.port)
            + L"\nPID: " + std::to_wstring(st.pid)
            + L"\n内存: " + FormatMem(st.memMB)
            + L"\n运行时长: " + FormatUptime(st.uptimeSec)
            + L"\n根目录: " + g_app.root;
        break;
    case Phase::Starting:
        msg = L"Hermes 正在启动中 (" + std::to_wstring(st.startingSec) + L"s / 45s)\n\n端口: 127.0.0.1:"
            + std::to_wstring(g_app.port) + L" 尚未就绪\n根目录: " + g_app.root;
        break;
    case Phase::Failed:
        msg = L"Hermes 已掉线 (端口无监听)\n\n端口: 127.0.0.1:" + std::to_wstring(g_app.port)
            + L"\n根目录: " + g_app.root;
        break;
    default:
        msg = std::wstring(g_app.rootState == RootState::PendingBuild
                               ? L"便携包尚未构建\n\n请先在「构建」页完成从0构建。\n根目录: "
                               : L"Hermes 未运行\n\n端口: 127.0.0.1:") + std::to_wstring(g_app.port)
            + L"\n根目录: " + g_app.root;
    }
    MessageBoxW(g_app.main, msg.c_str(), L"端口 / 状态", MB_OK | MB_ICONINFORMATION);
}

}  // namespace hs
