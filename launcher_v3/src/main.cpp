// main.cpp —— AgentDock 启动器 v3 入口 (ADR-007 生命周期架构)
//
// 三级启动探测:
//   完整根 (python/node 或 data+hermes-agent 指纹) -> Full 模式, 核心层全开
//   其余 (空目录/半成品)                            -> PendingBuild 模式,
//      首屏=构建向导, 核心功能延迟到构建成功后 InitCore 热初始化
// 壳功能 (FabBtn/托盘/热键/设置) 两模式均可用。
#include "Ui.h"
#include "WebViews.h"
#include "Brand.h"

#include <objbase.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include "TaskRunner.h"
#include "storage/JsonStore.h"
#include "core/DshRuntime.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

App g_app;
using namespace hs;

namespace {

constexpr UINT WM_TRAYICON_LOCAL = WM_APP + 30;

}  // namespace

// ---- App 路径助手 ----
// 启动器自有数据 (studio/) 一律锚定便携包根 (exe 目录), 不进任何 Agent 目录:
// 多 Agent 架构下 Agent 目录可能被更新/重建/删除, 启动器数据必须独立存活。
// Agent 目录只放 Agent 自己的数据 (data/, _home/, node/ ...)。
std::wstring App::StudioDirW() const {
    fs::path p = fs::path(exeDirW) / L"studio";
    std::error_code ec;
    fs::create_directories(p, ec);
    return p.wstring();
}

std::wstring App::PluginDirW() const {
    return fs::path(exeDirW) / L"plugin";
}

std::wstring App::LogsDirW() const {
    fs::path p = fs::path(StudioDirW()) / L"logs";
    std::error_code ec;
    fs::create_directories(p, ec);
    return p.wstring();
}

void App::LoadSettings() {
    settings = hs::AppSettings{};
    auto j = hs::JsonStore::load(fs::path(StudioDirW()) / "settings.json");
    if (j) {
        try { settings = j->get<hs::AppSettings>(); }
        catch (const std::exception&) { settings = hs::AppSettings{}; }
    }
    if (settings.runtime.webUiPort > 0) port = settings.runtime.webUiPort;
}

void App::SaveSettings() {
    nlohmann::json j = settings;
    (void)hs::JsonStore::save(fs::path(StudioDirW()) / "settings.json", j);
}

// 核心层延迟初始化: 启动时 Full 根调用 / 构建成功后再次调用 (ADR-007 零依赖启动)
bool App::InitCore(std::wstring& err) {
    if (!hs::Paths::autoDetect()) {
        err = L"根目录指纹校验未通过 (尚未构建完整便携包)";
        return false;
    }
    root = hs::Paths::root().wstring();
    rootState = RootState::Full;
    cfgPath = fs::path(exeDirW) / L"studio" / L"fab.json";

    LoadSettings();   // 根变化后重读 (可能换目录)
    proc.Init(root, port);

    // ModelRegistry (models.json; 缺失时尝试从旧 .env/config.yaml 迁移)
    delete registry;
    registry = new hs::ModelRegistry(hs::Paths::studioDir() / "models.json");
    (void)hs::JsonStore::ensureParent(hs::Paths::studioDir() / "models.json");
    auto lr = registry->load();
    if (!lr.ok()) {
        auto m = registry->migrateFromLegacy(hs::Paths::envFile(), hs::Paths::configFile(),
                                             hs::Paths::configTemplate());
        if (m.providersImported > 0 || m.modelsImported > 0) {
            (void)registry->save();
            logSink().Log(LogLevel::Info,
                          Utf8ToWide("已从旧配置迁移 " + std::to_string(m.providersImported)
                                     + " 个 Provider / " + std::to_string(m.modelsImported)
                                     + " 个模型"));
        }
    }

    // ConfigRenderer (.env / config.yaml)
    delete renderer;
    renderer = new hs::ConfigRenderer(hs::Paths::envFile(), hs::Paths::configFile(),
                                      hs::Paths::configTemplate());
    if (registry) {
        // 启动渲染守卫 (渲染是破坏性覆盖, 两个条件任一命中即止步):
        //  1) 注册表为空 (models.json 缺失/损坏且迁移拿到 0) -> 空渲染会把 Hermes 侧
        //     现役 .env/config.yaml 打成空壳 (实际事故: 每次启动清掉模型配置);
        //  2) 目标文件在渲染后被外部改过 (Hermes WebUI 内改的模型配置) -> 跳过,
        //     保留外部改动; 在 Provider/模型页显式保存时仍会全量渲染 (用户明确意图)。
        auto& mc = registry->config();
        if (mc.providers.empty() && mc.models.empty()) {
            logSink().Log(LogLevel::Warn,
                L"models.json 缺失或为空 - 跳过启动渲染, 现有 .env/config.yaml 保持不变 "
                L"(请在 Provider 页重新配置, 或恢复 studio/models.json 备份)");
        } else if (renderer->externallyModified()) {
            logSink().Log(LogLevel::Warn,
                L"检测到 .env/config.yaml 在启动器外部被修改 - 跳过启动渲染以保留改动 "
                L"(在启动器内保存 Provider/模型后将重新渲染)");
        } else {
            (void)renderer->renderAll(mc, settings.runtime, {});
        }
    }

    // 插件宿主 (core 完整版, 内部用 Paths::pluginDir)
    plugins.loadAll();
    logSink().Log(LogLevel::Info,
        L"[plugin] loadAll 完成: 已加载 " + std::to_wstring(plugins.loadedPlugins().size()) + L" 个");

    coreReady = true;
    logSink().Log(LogLevel::Info, L"核心层就绪: " + root);
    return true;
}

// ---- 托盘 ----
namespace hs {

void Tray_Add(HWND owner) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = owner;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON_LOCAL;
    nid.hIcon = (HICON)LoadImageW(g_app.inst, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                  GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                  LR_DEFAULTCOLOR);
    wcscpy_s(nid.szTip, HS_APP_NAME);
    Shell_NotifyIconW(NIM_ADD, &nid);
    g_app.trayAdded = true;
}

void Tray_Remove(HWND owner) {
    if (!g_app.trayAdded) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = owner;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_app.trayAdded = false;
}

void Tray_UpdateTip() {
    if (!g_app.main || !g_app.trayAdded) return;
    auto st = g_app.proc.Query();
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_app.main;
    nid.uID = 1;
    nid.uFlags = NIF_TIP;
    switch (st.phase) {
    case Phase::Running:
        swprintf_s(nid.szTip, HS_APP_NAME L" - 运行中 (PID %lu)", st.pid);
        break;
    case Phase::Starting:
        swprintf_s(nid.szTip, HS_APP_NAME L" - 正在启动中 (%llus)",
                   (unsigned long long)st.startingSec);
        break;
    case Phase::Failed:
        wcscpy_s(nid.szTip, HS_APP_NAME L" - 已掉线, 可从菜单重启");
        break;
    default:
        wcscpy_s(nid.szTip, g_app.rootState == RootState::PendingBuild
                                ? HS_APP_NAME L" - 待构建 (请先构建便携包)"
                                : HS_APP_NAME L" - 未启动");
    }
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void Tray_ShowMenu(HWND owner) {
    POINT pt;
    GetCursorPos(&pt);
    auto st = g_app.proc.Query();
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"打开启动器");
    AppendMenuW(m, MF_STRING, 2, L"打开 Hermes");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    switch (st.phase) {
    case Phase::Running:  AppendMenuW(m, MF_STRING, 4, L"停止 Hermes"); break;
    case Phase::Starting:
        AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"正在启动中...");
        AppendMenuW(m, MF_STRING, 4, L"停止 (中断启动)");
        break;
    default:
        if (g_app.rootState == RootState::Full)
            AppendMenuW(m, MF_STRING, 3, L"启动 Hermes");
        else
            AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"待构建 (请先在启动器中构建)");
        break;
    }
    AppendMenuW(m, MF_STRING, 6, L"查看日志");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 5, L"退出");
    SetForegroundWindow(owner);
    TrackPopupMenu(m, TPM_BOTTOMALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, 0, owner, nullptr);
    DestroyMenu(m);
}

void App_RefreshStatusUI() {
    MainWnd_Refresh();
    FabBtn_UpdateVisual();
    Tray_UpdateTip();
}

}  // namespace hs

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    using namespace hs;

    // 0) 互斥体防多开: 已有实例运行时, 前置其主窗口后退出。
    //    改名 (AgentDock) 过渡期: 旧品牌互斥体同样视为已运行, 避免新旧 exe 并存双开。
    wchar_t prevClassHit = 0;
    HANDLE oldMutex = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\HermesStudio.Launcher.SingleInstance");
    if (oldMutex) {
        CloseHandle(oldMutex);
        prevClassHit = 1;
    }
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\AgentDock.Launcher.SingleInstance");
    if ((mutex && GetLastError() == ERROR_ALREADY_EXISTS) || prevClassHit) {
        HWND prev = FindWindowW(L"HermesV3MainWnd", nullptr);
        if (prev) {
            ShowWindow(prev, SW_RESTORE);
            SetForegroundWindow(prev);
        }
        MessageBoxW(nullptr, HS_APP_NAME L" 启动器已在运行。", HS_APP_NAME,
                    MB_OK | MB_ICONINFORMATION);
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    g_app.inst = hInst;

    // 0.5) 自更新残留清理: 上次自替换留下的 .old.exe 保底副本 (applyLauncherUpdate),
    //      本次启动已确认旧版不再被锁, 直接删除。失败静默 (下次启动再试)。
    {
        wchar_t buf[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, buf, MAX_PATH)) {
            std::wstring oldExe = buf;
            size_t dot = oldExe.find_last_of(L'.');
            if (dot != std::wstring::npos) {
                oldExe = oldExe.substr(0, dot) + L".old.exe";
                DeleteFileW(oldExe.c_str());
            }
        }
    }

    // 1) DPI + 组件初始化
    ui::EnablePerMonitorDpi();
    Gdiplus::GdiplusStartupInput gi;
    ULONG_PTR gdiToken = 0;
    Gdiplus::GdiplusStartup(&gdiToken, &gi, nullptr);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // 2) exe 目录 (--root 可覆盖内容根)
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_app.exeDirW = exe;
    size_t slash = g_app.exeDirW.find_last_of(L"\\/");
    if (slash != std::wstring::npos) g_app.exeDirW = g_app.exeDirW.substr(0, slash);
    g_app.root = g_app.exeDirW;

    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv) {
            for (int i = 1; i + 1 < argc; ++i)
                if (_wcsicmp(argv[i], L"--root") == 0) g_app.root = argv[i + 1];
            LocalFree(argv);
        }
    }

    // 3) 三级启动探测: 完整根 -> Full; 否则 PendingBuild (待构建, 首屏=向导)
    {
        bool located = (g_app.root != g_app.exeDirW)
                         ? hs::Paths::setRoot(g_app.root)
                         : hs::Paths::autoDetect();
        // autoDetect 命中的真根必须同步回 g_app.root: cfgPath/托盘等直接用 g_app.root,
        // 不同步则 fab.json 落在 exeDir 而日志在真根\studio —— 读写分家, 窗口记忆失效
        if (located) g_app.root = hs::Paths::root().wstring();
        g_app.rootState = located ? RootState::Full : RootState::PendingBuild;
    }

    // 4) 日志 (Full: studio/logs/launcher.log; PendingBuild: exeDir/studio/logs)
    logSink().Init(g_app.LogsDirW() + L"\\launcher.log");
    logSink().Log(LogLevel::Info,
                  g_app.rootState == RootState::Full
                      ? std::wstring(HS_APP_NAME) + L" v" + HS_VERSION_W + L" 启动 (完整根: " + g_app.root + L")"
                      : std::wstring(HS_APP_NAME) + L" v" + HS_VERSION_W + L" 启动 (待构建模式)");

    // 4b) 一次性迁移: 启动器自有数据从 Hermes Agent 目录上移到便携包根。
    //     多 Agent 架构下 Agent 目录会被更新/重建/删除, 启动器数据
    //     (设置/模型/日志/备份/下载缓存/插件/窗口配置/WebView 配置) 必须独立存活。
    //     fab.json 曾在 data\config\ (Agent 数据目录内), 一并迁出。
    {
        std::error_code ec;
        fs::path hermesRoot = fs::path(g_app.root);
        fs::path portable = fs::path(g_app.exeDirW);
        auto moveTo = [&](const fs::path& from, const fs::path& to) -> bool {
            if (!fs::exists(from, ec) || fs::exists(to, ec)) return false;
            fs::create_directories(to.parent_path(), ec);
            fs::rename(from, to, ec);            // 同卷 rename
            if (ec) {                             // 跨卷兜底: 复制 + 删源
                ec.clear();
                fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
                if (!ec) fs::remove_all(from, ec);
            }
            return !ec;
        };
        // studio 根文件
        fs::path pStudio = portable / L"studio";
        fs::create_directories(pStudio, ec);
        for (const wchar_t* f : { L"settings.json", L"models.json", L"app.json",
                                  L"config.template.yaml" }) {
            if (moveTo(hermesRoot / L"studio" / f, pStudio / f))
                logSink().Log(LogLevel::Info, std::wstring(L"[mig] studio/") + f + L" 已上移到便携包根");
        }
        // studio 子目录
        for (const wchar_t* d : { L"logs", L"backups", L"cache" }) {
            if (moveTo(hermesRoot / L"studio" / d, pStudio / d))
                logSink().Log(LogLevel::Info, std::wstring(L"[mig] studio/") + d + L"/ 已上移到便携包根");
        }
        // 插件目录
        if (moveTo(hermesRoot / L"plugin", portable / L"plugin"))
            logSink().Log(LogLevel::Info, L"[mig] plugin/ 已上移到便携包根");
        // fab.json (曾在 Agent 数据目录 data\config\)
        if (moveTo(hermesRoot / L"data" / L"config" / L"fab.json", pStudio / L"fab.json"))
            logSink().Log(LogLevel::Info, L"[mig] data/config/fab.json 已上移到 portableRoot/studio");
        // WebView2 浏览器配置
        if (moveTo(hermesRoot / L"WebViewData", portable / L"WebViewData"))
            logSink().Log(LogLevel::Info, L"[mig] WebViewData/ 已上移到便携包根");
    }

    // 5) 悬浮按钮配置 + 端口
    g_app.cfgPath = g_app.StudioDirW() + L"\\fab.json";
    // fab.json 现固定在便携包根 studio\ (上方迁移已把历史位置搬来)
    g_app.cfg.Load(g_app.cfgPath);
    logSink().Log(LogLevel::Info,
        L"[cfg] path=" + g_app.cfgPath +
        L" wndX=" + std::to_wstring(g_app.cfg.wndX) +
        L" wndY=" + std::to_wstring(g_app.cfg.wndY) +
        L" wndW=" + std::to_wstring(g_app.cfg.wndW) +
        L" wndH=" + std::to_wstring(g_app.cfg.wndH));
    g_app.LoadSettings();
    g_app.proc.Init(g_app.root, g_app.port);
    // DSH 便携包子目录 (ADR-008): 与 Hermes 根无关, PendingBuild 下同样可用
    g_app.dshProc.Init(fs::path(g_app.exeDirW) / L"DSH", g_app.settings.dsh.webUiPort);
    logSink().Log(LogLevel::Info,
        L"[dsh] root=" + (fs::path(g_app.exeDirW) / L"DSH").wstring() +
        L" port=" + std::to_wstring(g_app.settings.dsh.webUiPort) +
        L" installed=" + (hs::dsh::installed() ? L"1" : L"0"));

    // 6) 核心层 (Full 根: 立即初始化; PendingBuild: 构建后热初始化)
    if (g_app.rootState == RootState::Full) {
        std::wstring err;
        if (!g_app.InitCore(err))
            logSink().Log(LogLevel::Error, L"核心层初始化失败: " + err);
    }

    g_app.wmTaskbarRestart = RegisterWindowMessageW(L"TaskbarCreated");

    // 7) 窗口与壳: 主面板 -> 桥/任务 -> WebView2 探测 -> 菜单 -> 悬浮按钮
    MainWnd_Create(nCmdShow);

    // 恢复窗口位置大小 (fab.json window 字段)
    logSink().Log(LogLevel::Info,
        L"[wnd] restore: wndX=" + std::to_wstring(g_app.cfg.wndX) +
        L" wndY=" + std::to_wstring(g_app.cfg.wndY) +
        L" wndW=" + std::to_wstring(g_app.cfg.wndW) +
        L" wndH=" + std::to_wstring(g_app.cfg.wndH));
    if (g_app.cfg.wndW > 200 && g_app.cfg.wndH > 100) {
        SetWindowPos(g_app.main, nullptr,
                     g_app.cfg.wndX, g_app.cfg.wndY,
                     g_app.cfg.wndW, g_app.cfg.wndH,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    Bridge_Init();
    TaskRunner::Instance().Init();
    // WebView2 用户数据目录锚定便携包根 (启动器自有, 不进 Agent 目录)
    Wv2_Init(g_app.main, g_app.exeDirW, g_app.port);
    FabMenu_Create();
    FabBtn_Create();
    FabBtn_ApplyVisibility();

    // 8) 托盘 + 全局热键 (兜底入口)
    Tray_Add(g_app.main);
    g_app.hotKeyOk = RegisterHotKey(g_app.main, g_app.hotKeyId, MOD_CONTROL | MOD_SHIFT, 'L') != 0;
    Tray_UpdateTip();

    // 9) launchOnStart: 自动启动 Hermes (消费 settings, FLTK/Qt6 行为)
    if (g_app.rootState == RootState::Full && g_app.settings.runtime.launchOnStart) {
        if (!g_app.proc.IsUp()) {
            std::wstring err;
            g_app.proc.Start(err);
        }
    }
    // DSH launchOnStart (ADR-008): 与 Hermes 独立
    if (g_app.settings.dsh.launchOnStart && hs::dsh::installed() && !g_app.dshProc.IsUp()) {
        std::wstring err;
        g_app.dshProc.Start(err);
    }
    // autoCheckUpdate: 0.8s 后自动远端检查一次 (FLTK 行为, 通道真实生效)。
    // 走 Bridge_AutoUpdateScan (与手动检查同路径, 含 DSH 组件行)
    if (g_app.rootState == RootState::Full && g_app.settings.runtime.autoCheckUpdate) {
        Bridge_AutoUpdateScan();
    }

    // 10) 消息循环
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // 11) 清理
    if (g_app.cfg.stopHermesOnExit && g_app.proc.IsUp()) g_app.proc.Stop();
    if (g_app.cfg.stopHermesOnExit && g_app.dshProc.IsUp()) g_app.dshProc.Stop();
    Tray_Remove(g_app.main);
    if (g_app.hotKeyOk) UnregisterHotKey(g_app.main, g_app.hotKeyId);
    g_app.cfg.Save(g_app.cfgPath);
    delete g_app.registry;
    delete g_app.renderer;
    if (gdiToken) Gdiplus::GdiplusShutdown(gdiToken);
    CoUninitialize();
    return (int)msg.wParam;
}
