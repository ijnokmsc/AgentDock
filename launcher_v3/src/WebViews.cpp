// WebViews.cpp —— WebView2 嵌入层实现
#include "Ui.h"
#include "WebViews.h"

#include <objbase.h>
#include "WebView2.h"
#include <algorithm>
#include <cstdio>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")

namespace hs {

namespace {

// ---- COM handler 基座 (与探针同套样板) ----
#define COM_BASE(T)                                                        \
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override { \
        if (riid == __uuidof(IUnknown) || riid == __uuidof(T)) { *ppv = this; return S_OK; } \
        *ppv = nullptr; return E_NOINTERFACE;                              \
    }                                                                      \
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }                \
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

struct State {
    Wv2State state = Wv2State::Probing;
    bool probeFired = false;                 // 探测导航事件已到
    HWND host = nullptr;
    std::wstring root, url1, url2, dataDir;
    RECT lastRc{ 0, 0, 0, 0 };               // 内容区 (由主窗 Wv2_Resize 维护)

    ICoreWebView2Environment* env = nullptr;
    ICoreWebView2Controller* ctl1 = nullptr;   // shell.local
    ICoreWebView2*           web1 = nullptr;
    ICoreWebView2Controller* ctl2 = nullptr;   // Hermes
    ICoreWebView2*           web2 = nullptr;
    bool hermesNavigated = false;
    ICoreWebView2Controller* ctl3 = nullptr;   // DSH (ADR-008)
    ICoreWebView2*           web3 = nullptr;
    std::wstring dshNavigatedUrl;              // 空 = 未导航; token 变化则重导航
    bool hermesExternal = false;               // 已重挂到独立窗口 (Hide/Resize 不得触碰)
    bool dshExternal = false;

    UINT_PTR probeTimerId = 0;
    bool     pendingVisualVerify = false;   // JS 验证过后等待屏幕像素终审
    UINT_PTR visualTimerId = 0;
};
State g;

// HRESULT -> "0xXXXXXXXX" (日志用)
std::string HrHex(HRESULT hr) {
    char b[16];
    sprintf_s(b, "0x%08X", (unsigned)hr);
    return b;
}

// ---- 单文件发布: 启动器页从 exe 资源内存伺服 (http://app.local/*, 零文件释放) ----
constexpr int IDR_SHELL_HTML = 301;
constexpr int IDR_SHELL_LOGO = 302;
constexpr wchar_t kAppHost[] = L"http://app.local";

// 取 exe 资源字节 (返回空 = 资源不存在)
std::vector<BYTE> LoadRcBytes(int resId) {
    HRSRC hr = FindResourceW(g_app.inst, MAKEINTRESOURCEW(resId), RT_RCDATA);
    if (!hr) return {};
    HGLOBAL h = LoadResource(g_app.inst, hr);
    if (!h) return {};
    DWORD size = SizeofResource(g_app.inst, hr);
    const void* data = LockResource(h);
    if (!data || !size) return {};
    return std::vector<BYTE>((const BYTE*)data, (const BYTE*)data + size);
}

// WebResourceRequested: 拦截 http://app.local/* 用内嵌资源应答 (不触网、不释放文件)
struct ShellResHandler : public ICoreWebView2WebResourceRequestedEventHandler {
    COM_BASE(ICoreWebView2WebResourceRequestedEventHandler)
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) override {
        if (!args || !g.env) return S_OK;
        ICoreWebView2WebResourceRequest* req = nullptr;
        LPWSTR uri = nullptr;
        if (FAILED(args->get_Request(&req)) || !req) return S_OK;
        HRESULT gur = req->get_Uri(&uri);
        req->Release();
        if (FAILED(gur) || !uri) return S_OK;
        std::wstring path = uri;
        CoTaskMemFree(uri);
        size_t q = path.find_first_of(L"?#");
        if (q != std::wstring::npos) path.resize(q);
        logSink().Log(LogLevel::Info, Utf8ToWide("[wv2] WebResourceRequested: " + WideToUtf8(path)));
        std::wstring tail = path.c_str() + wcslen(kAppHost);

        int resId = 0; const wchar_t* mime = L"text/html; charset=utf-8";
        if (tail == L"/" || tail == L"/index.html") resId = IDR_SHELL_HTML;
        else if (tail == L"/logo.png") { resId = IDR_SHELL_LOGO; mime = L"image/png"; }
        if (!resId) return S_OK;   // 其余交默认处理 (404)

        std::vector<BYTE> bytes = LoadRcBytes(resId);
        if (bytes.empty()) { logSink().Log(LogLevel::Warn, L"[wv2] 资源为空!"); return S_OK; }
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
        if (!h) return S_OK;
        memcpy(GlobalLock(h), bytes.data(), bytes.size());
        GlobalUnlock(h);
        IStream* stream = nullptr;
        if (FAILED(CreateStreamOnHGlobal(h, TRUE, &stream))) { GlobalFree(h); return S_OK; }
        ICoreWebView2WebResourceResponse* resp = nullptr;
        HRESULT hr = g.env->CreateWebResourceResponse(stream, 200, L"OK", mime, &resp);
        stream->Release();
        if (SUCCEEDED(hr) && resp) {
            HRESULT pr = args->put_Response(resp);
            resp->Release();
            logSink().Log(LogLevel::Info, Utf8ToWide("[wv2] 内存应答 " + WideToUtf8(path)
                                                   + " bytes=" + std::to_string(bytes.size())
                                                   + " put_Response hr=" + HrHex(pr)));
        } else {
            logSink().Log(LogLevel::Warn, Utf8ToWide("[wv2] CreateWebResourceResponse hr=" + HrHex(hr)));
        }
        return S_OK;
    }
};
static ShellResHandler s_shellRes;

void MarkProbeFired() {
    if (g.probeFired) return;
    g.probeFired = true;
}

void SetAvailable();   // 定义见下 (探测结论)
void SetUnavailable(const char* reason);
void CALLBACK VisualVerifyTimerProc(HWND, UINT, UINT_PTR, DWORD);

struct NavHandler : public ICoreWebView2NavigationStartingEventHandler {
    COM_BASE(ICoreWebView2NavigationStartingEventHandler)
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs*) override {
        MarkProbeFired();                     // 浏览器侧在干活 (仅参考, 不作结论)
        return S_OK;
    }
};
static NavHandler s_navStart;

// 探测终极判据: ExecuteScript 在渲染器里跑 JS 且结果回传 (证明 JS/IPC 全链路活着)
struct ProbeScriptHandler : public ICoreWebView2ExecuteScriptCompletedHandler {
    COM_BASE(ICoreWebView2ExecuteScriptCompletedHandler)
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, LPCWSTR resultJson) override {
        std::string res = (SUCCEEDED(hr) && resultJson) ? WideToUtf8(resultJson) : "(null)";
        logSink().Log(LogLevel::Info,
                      Utf8ToWide("[wv2] ExecuteScript 回调 hr=" + HrHex(hr) + " result=" + res));
        if (SUCCEEDED(hr) && resultJson && wcscmp(resultJson, L"\"probe6x7\"") == 0) {
            SetAvailable();
            // 最终判据 = 屏幕像素: 暂判可用并显示 #1, 2.5s 后验证内容区真有渲染。
            if (Wv2_ShowShell()) {
                g.pendingVisualVerify = true;
                g.visualTimerId = SetTimer(g.host, 0, 2500, VisualVerifyTimerProc);
            } else {
                SetUnavailable("ShowShell failed after script probe");
            }
        }
        // 否则静默: 等超时判 Unavailable
        return S_OK;
    }
};
static ProbeScriptHandler s_probeScript;

// #1 (shell.local) 专用: NavigationCompleted -> 发起脚本验证
struct NavShellDoneHandler : public ICoreWebView2NavigationCompletedEventHandler {
    COM_BASE(ICoreWebView2NavigationCompletedEventHandler)
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* a) override {
        MarkProbeFired();
        BOOL ok = FALSE;
        if (a) a->get_IsSuccess(&ok);
        logSink().Log(LogLevel::Info,
                      Utf8ToWide(std::string("[wv2] shell.local NavigationCompleted isSuccess=") + (ok ? "1" : "0")));
        // 导航完成后重推插件列表: 页面早期发出的 plugin.list 响应可能落在
        // 导航窗口期被 ExecuteScript 静默丢弃 (侧边栏入口缺失的根因)
        Bridge_OnShellNavigationCompleted();
        if (g.web1 && g.state == Wv2State::Probing)
            g.web1->ExecuteScript(L"'probe6x7'", &s_probeScript);
        return S_OK;
    }
};
static NavShellDoneHandler s_navShellDone;

struct NavDoneHandler : public ICoreWebView2NavigationCompletedEventHandler {
    COM_BASE(ICoreWebView2NavigationCompletedEventHandler)
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) override {
        MarkProbeFired();
        return S_OK;
    }
};
static NavDoneHandler s_navDone;

struct SrcHandler : public ICoreWebView2SourceChangedEventHandler {
    COM_BASE(ICoreWebView2SourceChangedEventHandler)
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*, ICoreWebView2SourceChangedEventArgs*) override {
        MarkProbeFired();
        return S_OK;
    }
};
static SrcHandler s_srcChg;

// 探测结论 -> Available
void SetAvailable() {
    if (g.state != Wv2State::Probing) return;
    g.state = Wv2State::Available;
    if (g.probeTimerId) { KillTimer(g.host, g.probeTimerId); g.probeTimerId = 0; }
    logSink().Log(LogLevel::Info, L"[wv2] 探测通过 -> Available");
    PostMessageW(g.host, WM_WV2_STATE, 1, 0);
}

void SetUnavailable(const char* reason) {
    if (g.state == Wv2State::Unavailable) return;
    if (g.probeTimerId) { KillTimer(g.host, g.probeTimerId); g.probeTimerId = 0; }
    if (g.visualTimerId) { KillTimer(g.host, g.visualTimerId); g.visualTimerId = 0; }
    g.pendingVisualVerify = false;
    g.state = Wv2State::Unavailable;
    logSink().Log(LogLevel::Warn, Utf8ToWide(std::string("[wv2] 判定不可用: ") + reason));
    // 释放半成品
    if (g.ctl1) { g.ctl1->Close(); g.ctl1->Release(); g.ctl1 = nullptr; g.web1 = nullptr; }
    if (g.ctl2) { g.ctl2->Close(); g.ctl2->Release(); g.ctl2 = nullptr; g.web2 = nullptr; }
    if (g.ctl3) { g.ctl3->Close(); g.ctl3->Release(); g.ctl3 = nullptr; g.web3 = nullptr; }
    if (g.env)  { g.env->Release(); g.env = nullptr; }
    PostMessageW(g.host, WM_WV2_STATE, 0, 0);
}

// 屏幕像素终审: 内容区接近纯色 (无渲染输出) -> 回退原生。证据全部落日志。
void CALLBACK VisualVerifyTimerProc(HWND, UINT, UINT_PTR, DWORD) {
    if (!g.pendingVisualVerify) return;
    if (!IsWindowVisible(g.host)) {
        g.visualTimerId = SetTimer(g.host, 0, 3000, VisualVerifyTimerProc);   // 窗口隐藏时延后
        return;
    }
    g.pendingVisualVerify = false;
    if (g.visualTimerId) { KillTimer(g.host, g.visualTimerId); g.visualTimerId = 0; }

    // 内容区 (lastRc 为客户区坐标 -> 转屏幕)
    POINT org{ 0, 0 };
    ClientToScreen(g.host, &org);
    int x = org.x + g.lastRc.left, y = org.y + g.lastRc.top;
    int w = g.lastRc.right - g.lastRc.left, h = g.lastRc.bottom - g.lastRc.top;
    wchar_t buf[160];
    swprintf_s(buf, L"[wv2] 像素终审: lastRc=(%ld,%ld,%ld,%ld) 采样区=(%d,%d %dx%d)",
               (long)g.lastRc.left, (long)g.lastRc.top, (long)g.lastRc.right, (long)g.lastRc.bottom,
               x, y, w, h);
    logSink().Log(LogLevel::Info, buf);
    if (w < 80 || h < 80) { SetUnavailable("content rect too small"); return; }

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY | CAPTUREBLT);

    // 抽样统计众数颜色占比 (跳过边缘 16px 避免页签条/状态栏渗色)
    struct Cnt { COLORREF c; int n; };
    Cnt counts[64]{};
    int total = 0;
    for (int px = 16; px < w - 16; px += 7) {
        for (int py = 16; py < h - 16; py += 7) {
            COLORREF c = GetPixel(mem, px, py);
            total++;
            for (auto& e : counts) {
                if (e.n == 0) { e.c = c; e.n = 1; break; }
                if (e.c == c) { e.n++; break; }
            }
        }
    }
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    int top = 0; COLORREF mode = 0;
    for (auto& e : counts) if (e.n > top) { top = e.n; mode = e.c; }
    int ratio = total ? top * 1000 / total : 1000;
    swprintf_s(buf, L"[wv2] 像素终审: 主色占比=%d.%d%% 主色=#%06X (判定阈值 99.5%%)",
               ratio / 10, ratio % 10, (unsigned)mode);
    logSink().Log(LogLevel::Info, buf);
    if (total > 0 && top * 1000 > total * 995) {
        SetUnavailable("pixel verify: single-color content (blank render)");
    }
    // 否则维持 Available (主窗已收到 WM_WV2_STATE(1))
}

// 探测定时器 (host 窗口): 10s 内脚本验证通过 -> Available (SetAvailable 由
// ProbeScriptHandler 调用); 否则一律 Unavailable (仅导航事件不作数)
void CALLBACK ProbeTimerProc(HWND h, UINT, UINT_PTR id, DWORD) {
    if (g.state != Wv2State::Probing) return;
    SetUnavailable("probe timeout 10s (no script verdict)");
    (void)h; (void)id;
}

// ---- #1 (shell.local) 的消息桥: 页面 postMessage({cmd:...}) -> Bridge 分发 ----
struct MsgHandler : public ICoreWebView2WebMessageReceivedEventHandler {
    COM_BASE(ICoreWebView2WebMessageReceivedEventHandler)
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* e) override {
        if (!e) return S_OK;
        LPWSTR json = nullptr;
        if (SUCCEEDED(e->get_WebMessageAsJson(&json)) && json) {
            Bridge_HandleJson(WideToUtf8(json));
            CoTaskMemFree(json);
        }
        return S_OK;
    }
};
static MsgHandler s_msg;

void WireProbeEvents(ICoreWebView2* web) {
    EventRegistrationToken tok{};
    web->add_NavigationStarting(&s_navStart, &tok);
    web->add_NavigationCompleted(&s_navDone, &tok);
    web->add_SourceChanged(&s_srcChg, &tok);
}

struct CtrlDshHandler : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
    COM_BASE(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Controller* ctrl) override {
        logSink().Log(LogLevel::Info, Utf8ToWide("[wv2] ctrl#3 (DSH) 回调 hr=" + HrHex(hr)));
        if (FAILED(hr) || !ctrl) return S_OK;   // #3 失败不否定整体可用性
        g.ctl3 = ctrl; ctrl->AddRef();
        ctrl->get_CoreWebView2(&g.web3);
        WireProbeEvents(g.web3);
        g.ctl3->put_IsVisible(FALSE);
        return S_OK;
    }
};
static CtrlDshHandler s_ctrlDsh;

struct CtrlHermesHandler : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
    COM_BASE(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Controller* ctrl) override {
        logSink().Log(LogLevel::Info, Utf8ToWide("[wv2] ctrl#2 (Hermes) 回调 hr=" + HrHex(hr)));
        if (FAILED(hr) || !ctrl) { SetUnavailable("ctrl2 failed"); return S_OK; }
        g.ctl2 = ctrl; ctrl->AddRef();
        ctrl->get_CoreWebView2(&g.web2);
        WireProbeEvents(g.web2);
        g.ctl2->put_IsVisible(FALSE);       // 默认隐藏在 #1 之下
        // #3 (DSH) 同 env 串行创建; 不挂任何宿主接口 (与 #2 同一安全边界)
        g.env->CreateCoreWebView2Controller(g.host, &s_ctrlDsh);
        return S_OK;
    }
};
static CtrlHermesHandler s_ctrlHermes;

struct CtrlShellHandler : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
    COM_BASE(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Controller* ctrl) override {
        logSink().Log(LogLevel::Info, Utf8ToWide("[wv2] ctrl#1 (shell.local) 回调 hr=" + HrHex(hr)));
        if (FAILED(hr) || !ctrl) { SetUnavailable("ctrl1 failed"); return S_OK; }
        g.ctl1 = ctrl; ctrl->AddRef();
        ctrl->get_CoreWebView2(&g.web1);
        // #1: 只对启动器页启用消息桥; Hermes 视图不挂任何宿主接口 (ADR-007 安全边界)。
        // 完成事件用专用 handler: 导航完成后发 ExecuteScript 做最终验证。
        EventRegistrationToken tok{};
        g.web1->add_WebMessageReceived(&s_msg, &tok);
        g.web1->add_NavigationStarting(&s_navStart, &tok);
        g.web1->add_SourceChanged(&s_srcChg, &tok);
        g.web1->add_NavigationCompleted(&s_navShellDone, &tok);
        // 单文件发布: http://app.local/* 由 exe 内嵌资源应答 (须在 Navigate 前注册;
        // AddWebResourceRequestedFilter 声明过滤 + add_WebResourceRequested 挂 handler, 二者缺一不可)
        g.web1->AddWebResourceRequestedFilter(L"http://app.local/*",
                                              COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
        g.web1->add_WebResourceRequested(&s_shellRes, &tok);
        // 注意: 不再 put_IsVisible(FALSE) —— 对照实验 (wv2test b3) 证明本机
        // "创建即不可见 -> 后转可见" 不渲染, 从头可见才正常; 探测期它就是 UI。
        HRESULT nh = g.web1->Navigate(g.url1.c_str());
        logSink().Log(LogLevel::Info, Utf8ToWide("[wv2] Navigate(shell.local) hr=" + HrHex(nh)
                                               + " url=" + WideToUtf8(g.url1)));

        // #2 同 env 创建 (不预导航, ShowHermes 时再 Navigate)
        g.env->CreateCoreWebView2Controller(g.host, &s_ctrlHermes);

        // 探测定时器: 最多等 10s
        g.probeTimerId = SetTimer(g.host, 0, 10000, ProbeTimerProc);
        return S_OK;
    }
};
static CtrlShellHandler s_ctrlShell;

struct EnvHandler : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
    COM_BASE(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Environment* env) override {
        logSink().Log(LogLevel::Info, Utf8ToWide("[wv2] env 回调 hr=" + HrHex(hr)));
        if (FAILED(hr) || !env) { SetUnavailable("env failed"); return S_OK; }
        g.env = env; env->AddRef();
        return env->CreateCoreWebView2Controller(g.host, &s_ctrlShell);
    }
};
static EnvHandler s_env;

}  // namespace

Wv2State Wv2_GetState() { return g.state; }

void Wv2_Init(HWND host, const std::wstring& root, int port) {
    g.host = host;
    g.root = root;
    g.dataDir = root + L"\\WebViewData";
    CreateDirectoryW(g.dataDir.c_str(), nullptr);
    // 单文件发布: 启动器页编译进 exe, 经虚拟域名内存伺服 (零文件释放)
    g.url1 = L"http://app.local/index.html";
    g.url2 = L"http://127.0.0.1:" + std::to_wstring(port) + L"/";

    // browserExecutableFolder: 便携 fixed 版优先, 其次 Evergreen
    std::wstring fixed = root + L"\\webview2";
    DWORD attr = GetFileAttributesW((fixed + L"\\msedgewebview2.exe").c_str());
    const wchar_t* browserFolder =
        (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) ? fixed.c_str() : nullptr;

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        browserFolder, g.dataDir.c_str(), nullptr, &s_env);
    logSink().Log(LogLevel::Info,
                  Utf8ToWide(std::string("[wv2] Wv2_Init: browserFolder=")
                           + (browserFolder ? WideToUtf8(fixed) : "(Evergreen)")
                           + " dataDir=" + WideToUtf8(g.dataDir)
                           + " sync hr=" + HrHex(hr)));
    if (FAILED(hr)) SetUnavailable("CreateEnvironment sync failed");
}

bool Wv2_ShowShell() {
    if (g.state != Wv2State::Available || !g.ctl1 || !g.ctl2) return false;
    g.ctl1->put_Bounds(g.lastRc);
    g.ctl1->put_IsVisible(TRUE);
    if (g.ctl2 && !g.hermesExternal) g.ctl2->put_IsVisible(FALSE);
    if (g.ctl3 && !g.dshExternal)    g.ctl3->put_IsVisible(FALSE);
    return true;
}

bool Wv2_ShowHermes() {
    if (g.state != Wv2State::Available || !g.ctl1 || !g.ctl2) return false;
    if (g.hermesExternal) return false;   // 已拆出独立窗, 主窗不嵌入
    if (!g.hermesNavigated) {
        g.web2->Navigate(g.url2.c_str());
        g.hermesNavigated = true;
    }
    g.ctl2->put_Bounds(g.lastRc);
    g.ctl2->put_IsVisible(TRUE);
    g.ctl1->put_IsVisible(FALSE);
    if (g.ctl3 && !g.dshExternal) g.ctl3->put_IsVisible(FALSE);
    return true;
}

bool Wv2_ShowDsh(const std::wstring& desiredUrl) {
    if (g.state != Wv2State::Available || !g.ctl1 || !g.ctl2 || !g.ctl3) return false;
    if (g.dshExternal) return false;      // 已拆出独立窗, 主窗不嵌入
    if (desiredUrl.empty()) return false;
    // token 每 boot 变化: URL 与上次不同 (含首次) 则重导航
    if (g.dshNavigatedUrl != desiredUrl) {
        g.web3->Navigate(desiredUrl.c_str());
        g.dshNavigatedUrl = desiredUrl;
        logSink().Log(LogLevel::Info, L"[wv2] Navigate(DSH) url 含 token="
                                      + std::to_wstring(desiredUrl.find(L"token=") != std::wstring::npos));
    }
    g.ctl3->put_Bounds(g.lastRc);
    g.ctl3->put_IsVisible(TRUE);
    g.ctl1->put_IsVisible(FALSE);
    if (g.ctl2 && !g.hermesExternal) g.ctl2->put_IsVisible(FALSE);
    return true;
}

// ---- 独立窗口: 重挂父窗口 (官方 put_ParentWindow) ----
// 重挂到非主窗目标后标记 external: Wv2_Hide/Wv2_Resize 不再触碰该视图,
// 否则主窗页签切换会把独立窗口的内容一起隐藏/改尺寸。
// 拆分 (external) 时同时点亮控制器 —— 此前页签切换把它 put_IsVisible(FALSE),
// 重挂不会自动恢复可见, 独立窗里会是空的。
template <typename CtlT>
static HRESULT Reparent(CtlT* ctl, HWND host, const RECT& rc, bool& external) {
    if (!ctl) return E_POINTER;
    external = (host != g.host);
    HRESULT hr = ctl->put_ParentWindow(host);
    if (SUCCEEDED(hr)) {
        ctl->put_Bounds(rc);
        if (external) ctl->put_IsVisible(TRUE);
    }
    return hr;
}

bool Wv2_ReparentHermes(HWND newHost) {
    if (!g.ctl2) return false;
    RECT rc = g.lastRc;
    HRESULT hr = Reparent(g.ctl2, newHost, rc, g.hermesExternal);
    logSink().Log(LogLevel::Info, Utf8ToWide(std::string("[wv2] ReparentHermes hr=") + HrHex(hr)
                                           + " external=" + (g.hermesExternal ? "1" : "0")));
    return SUCCEEDED(hr);
}

bool Wv2_ReparentDsh(HWND newHost) {
    if (!g.ctl3) return false;
    RECT rc = g.lastRc;
    HRESULT hr = Reparent(g.ctl3, newHost, rc, g.dshExternal);
    logSink().Log(LogLevel::Info, Utf8ToWide(std::string("[wv2] ReparentDsh hr=") + HrHex(hr)
                                           + " external=" + (g.dshExternal ? "1" : "0")));
    return SUCCEEDED(hr);
}

void Wv2_SetHermesBounds(const RECT& rc) { if (g.ctl2 && g.hermesExternal) g.ctl2->put_Bounds(rc); }
void Wv2_SetDshBounds(const RECT& rc)    { if (g.ctl3 && g.dshExternal)   g.ctl3->put_Bounds(rc); }
bool Wv2_HasHermesView() { return g.ctl2 != nullptr; }
bool Wv2_HasDshView()    { return g.ctl3 != nullptr; }
void Wv2_NavigateDsh(const std::wstring& url) {
    if (!g.web3 || url.empty()) return;
    g.web3->Navigate(url.c_str());
    g.dshNavigatedUrl = url;
}

void Wv2_NavigateHermes() {
    g.hermesNavigated = false;
    if (g.web2) g.web2->Navigate(g.url2.c_str());
    g.hermesNavigated = true;
}

void Wv2_Hide() {
    if (g.ctl1) g.ctl1->put_IsVisible(FALSE);
    if (g.ctl2 && !g.hermesExternal) g.ctl2->put_IsVisible(FALSE);
    if (g.ctl3 && !g.dshExternal)    g.ctl3->put_IsVisible(FALSE);
}

void Wv2_Resize(const RECT& rc) {
    g.lastRc = rc;                       // 供像素终审/Show* 使用 (此前漏存导致空矩形误判)
    if (g.ctl1) g.ctl1->put_Bounds(rc);
    if (g.ctl2 && !g.hermesExternal) g.ctl2->put_Bounds(rc);
    if (g.ctl3 && !g.dshExternal)    g.ctl3->put_Bounds(rc);
}

void Wv2_PushEventJson(const std::string& jsonUtf8) {
    if (g.state != Wv2State::Available || !g.web1) {
        static int dropLogged = 0;
        if (dropLogged++ < 3)
            logSink().Log(LogLevel::Warn,
                          Utf8ToWide("[wv2] PushEvent 丢弃: state=" + std::to_string((int)g.state)
                                   + " web1=" + (g.web1 ? "ok" : "null")));
        return;
    }
    std::wstring script = L"window.__hostEvent && window.__hostEvent("
                        + Utf8ToWide(jsonUtf8) + L")";
    g.web1->ExecuteScript(script.c_str(), nullptr);
}

}  // namespace hs
