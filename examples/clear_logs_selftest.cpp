// clear-logs 插件自测: 加载 plugin.dll, 在沙箱树里跑 扫描/选择清理/一键清理,
// 校验分类统计、风险默认、只动所选类别。结论以退出码输出 (0=PASS)。
//
// 编译: cl /I<hs_plugin.h 目录> clear_logs_selftest.cpp /Fe:clear_logs_selftest.exe
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <windows.h>
#include "hs_plugin.h"

typedef const hs_plugin_api_vtable* (*EntryFn)(void);

static std::string g_root;                 // 沙箱便携包根
static std::string g_lastData;             // 最近一条 @@DATA@@ 载荷
static std::vector<std::string> g_logs;

static void fakeLog(hs_log_level, const char* tag, const char* msg) {
    std::string m = (tag ? tag : "") + std::string(": ") + (msg ? msg : "");
    auto p = m.find("@@DATA@@");
    if (p != std::string::npos) { g_lastData = m.substr(p + 8); return; }
    g_logs.push_back(m);
}
static hs_i32 fakeConfigGet(const char* key, const char** out) {
    static std::string buf;
    if (strcmp(key, "hermesRoot") == 0) {
        // CLREAL=1: 只读实测模式, 指向真实便携包根 (扫描后直接退出, 不清理)
        if (getenv("CLREAL")) buf = "\"I:\\PortableAgent\\Hermes\"";
        else buf = "\"" + g_root + "\\Hermes\"";     // 内容根 = 沙箱\Hermes (便携根为其上级)
        *out = buf.c_str();
        return 0;
    }
    return -1;
}
static void fakeStringFree(const char*) {}

// ---- 沙箱断言 ----
static int g_fail = 0;
#define CHECK(cond, msg) do { if (cond) printf("  [OK] %s\n", msg); \
    else { printf("  [FAIL] %s\n", msg); ++g_fail; } } while (0)

static bool pathExists(const std::string& p) {
    return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}
static void mkDir(const std::string& p) { CreateDirectoryA(p.c_str(), nullptr); }
static void mkFile(const std::string& p, DWORD size) {
    HANDLE h = CreateFileA(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) { printf("  [FAIL] cannot create %s\n", p.c_str()); exit(2); }
    SetFilePointer(h, (LONG)size, nullptr, FILE_BEGIN);
    SetEndOfFile(h);
    CloseHandle(h);
}
// 递归统计某目录下剩余文件数
static int countFiles(const std::string& dir) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        std::string full = dir + "\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) n += countFiles(full);
        else ++n;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

// 在 json 文本里找 "id":"xxx" 类别的 "bytes":N
static long long jsonCatBytes(const std::string& js, const char* id) {
    std::string key = std::string("\"id\":\"") + id + "\"";
    size_t p = js.find(key);
    if (p == std::string::npos) return -1;
    size_t b = js.find("\"bytes\":", p);
    if (b == std::string::npos) return -1;
    return atoll(js.c_str() + b + 8);
}
static bool jsonHasCat(const std::string& js, const char* id) {
    return js.find("\"id\":\"" + std::string(id) + "\"") != std::string::npos;
}

int main() {
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    std::string base = std::string(tmp) + "clselftest";
    g_root = base + "\\pkg";
    // 重建沙箱
    system(("rd /s /q \"" + base + "\" >nul 2>&1").c_str());
    mkDir(base);
    mkDir(g_root);
    mkFile(g_root + "\\AgentDock.exe", 0);     // 便携根锚点 (插件靠它向上探测)
    const std::string R = g_root;
    // 便携包根直下 (启动器自身)
    mkDir(R + "\\studio"); mkDir(R + "\\studio\\logs"); mkDir(R + "\\studio\\backups");
    mkDir(R + "\\studio\\cache"); mkDir(R + "\\studio\\cache\\downloads");
    mkFile(R + "\\studio\\logs\\launcher.log", 4000);
    mkFile(R + "\\studio\\backups\\webui-pre-0.7.0.zip", 500000);
    mkFile(R + "\\studio\\cache\\downloads\\hermes-runtime.tar.gz", 2000000);
    mkDir(R + "\\WebViewData"); mkDir(R + "\\WebViewData\\EBWebView");
    mkDir(R + "\\WebViewData\\EBWebView\\Default"); mkDir(R + "\\WebViewData\\EBWebView\\Default\\Cache");
    mkFile(R + "\\WebViewData\\EBWebView\\Default\\Cache\\f_000001", 100000);
    mkDir(R + "\\WebViewData\\EBWebView\\Default\\Cookies");   // 用户数据目录名不在白名单
    mkFile(R + "\\WebViewData\\EBWebView\\Default\\Cookies\\db", 100);
    mkFile(R + "\\crash.dmp", 3000);
    // Hermes 子包
    mkDir(R + "\\Hermes"); mkDir(R + "\\Hermes\\_home"); mkDir(R + "\\Hermes\\_home\\AppData");
    mkDir(R + "\\Hermes\\_home\\AppData\\Local"); mkDir(R + "\\Hermes\\_home\\AppData\\Local\\npm-cache");
    mkFile(R + "\\Hermes\\_home\\AppData\\Local\\npm-cache\\cacache.bin", 1000000);
    mkDir(R + "\\Hermes\\_home\\.hermes-web-ui"); mkDir(R + "\\Hermes\\_home\\.hermes-web-ui\\logs");
    mkFile(R + "\\Hermes\\_home\\.hermes-web-ui\\logs\\bridge.log", 8000);
    mkDir(R + "\\Hermes\\_home\\tmp"); mkFile(R + "\\Hermes\\_home\\tmp\\x.tmp", 2000);
    mkDir(R + "\\Hermes\\data"); mkDir(R + "\\Hermes\\data\\logs"); mkDir(R + "\\Hermes\\data\\cache");
    mkFile(R + "\\Hermes\\data\\logs\\app.log", 60000);
    mkFile(R + "\\Hermes\\data\\cache\\t.json", 5000);
    mkFile(R + "\\Hermes\\data\\models_dev_cache.json", 40000);
    mkDir(R + "\\Hermes\\python"); mkDir(R + "\\Hermes\\python\\pkg"); mkDir(R + "\\Hermes\\python\\pkg\\__pycache__");
    mkFile(R + "\\Hermes\\python\\pkg\\__pycache__\\m.pyc", 7000);
    // node_modules 里长得像垃圾的东西必须不被碰
    mkDir(R + "\\Hermes\\webui"); mkDir(R + "\\Hermes\\webui\\node_modules"); mkDir(R + "\\Hermes\\webui\\node_modules\\pkga");
    mkDir(R + "\\Hermes\\webui\\node_modules\\pkga\\logs");
    mkFile(R + "\\Hermes\\webui\\node_modules\\pkga\\logs\\a.log", 900);
    // DSH 子包
    mkDir(R + "\\DSH"); mkDir(R + "\\DSH\\studio"); mkDir(R + "\\DSH\\studio\\cache");
    mkDir(R + "\\DSH\\studio\\cache\\pnpm"); mkFile(R + "\\DSH\\studio\\cache\\pnpm\\store.bin", 3000000);
    mkDir(R + "\\DSH\\_home"); mkDir(R + "\\DSH\\_home\\AppData"); mkDir(R + "\\DSH\\_home\\AppData\\Local");
    mkDir(R + "\\DSH\\_home\\AppData\\Local\\node-gyp"); mkFile(R + "\\DSH\\_home\\AppData\\Local\\node-gyp\\g.bin", 50000);

    // 加载 DLL (与 selftest 同目录, 构建脚本保证)
    char self[MAX_PATH]; GetModuleFileNameA(nullptr, self, MAX_PATH);
    std::string selfDir = self; { size_t p = selfDir.find_last_of("\\/"); selfDir = selfDir.substr(0, p); }
    std::string dllPath = selfDir + "\\clear_logs_plugin.dll";
    HMODULE hDll = LoadLibraryA(dllPath.c_str());
    if (!hDll) { printf("[FAIL] LoadLibrary %s (%lu)\n", dllPath.c_str(), GetLastError()); return 2; }
    auto entry = (EntryFn)GetProcAddress(hDll, "hs_plugin_entry");
    if (!entry) { printf("[FAIL] hs_plugin_entry not found\n"); return 2; }
    const hs_plugin_api_vtable* api = entry();
    if (!api || api->abi_version != HS_ABI_VERSION) { printf("[FAIL] bad abi\n"); return 2; }

    static hs_host_api_vtable host = {};
    host.api_version = HS_HOST_API_VERSION;
    host.log = fakeLog;
    host.config_get = fakeConfigGet;
    host.string_free = fakeStringFree;
    void* ud = nullptr;
    if (api->create(&host, &ud) != 0) { printf("[FAIL] create\n"); return 2; }

    printf("== scan ==\n");
    g_lastData.clear(); g_logs.clear();
    int rc = api->execute_action(ud, "scan", "{}");
    CHECK(rc == 0, "scan rc==0");
    CHECK(!g_lastData.empty(), "scan 输出 @@DATA@@ checklist");
    if (getenv("CLDEBUG")) printf("DATA=%s\n", g_lastData.c_str());
    if (getenv("CLREAL")) {                       // 实测模式: 打印分类结果即退出, 不做任何清理
        printf("logs:\n");
        for (auto& l : g_logs) printf("  %s\n", l.c_str());
        api->destroy(ud);
        return rc == 0 ? 0 : 1;
    }
    const std::string& js = g_lastData;
    CHECK(js.find("\"root\":\"") != std::string::npos &&
          js.find("pkg") != std::string::npos, "checklist.root 为沙箱便携根");
    CHECK(jsonHasCat(js, "upd_dl") && jsonCatBytes(js, "upd_dl") == 2000000, "upd_dl=2000000");
    CHECK(jsonHasCat(js, "backups") && jsonCatBytes(js, "backups") == 500000, "backups=500000");
    CHECK(jsonHasCat(js, "pkgcache") && jsonCatBytes(js, "pkgcache") == 4050000, "pkgcache=pnpm 3000000+npm 1000000+node-gyp 50000");
    CHECK(jsonHasCat(js, "webcache") && jsonCatBytes(js, "webcache") == 100000, "webcache=Cache 100000 (Cookies 不计)");
    CHECK(jsonHasCat(js, "logs") && jsonCatBytes(js, "logs") == 72000, "logs=4000+8000+60000");
    CHECK(jsonHasCat(js, "pycache") && jsonCatBytes(js, "pycache") == 7000, "pycache=7000");
    CHECK(jsonHasCat(js, "temp") && jsonCatBytes(js, "temp") == 5000, "temp=2000+3000");
    CHECK(jsonHasCat(js, "datacache") && jsonCatBytes(js, "datacache") == 45000, "datacache=5000+40000");
    CHECK(js.find("node_modules") == std::string::npos, "目标里不出现 node_modules");
    CHECK(g_lastData.find("risk\":0") != std::string::npos, "风险字段存在");

    printf("== clean 勾选项 (logs+pycache) ==\n");
    int logsBefore = countFiles(R + "\\Hermes\\webui\\node_modules\\pkga\\logs");
    g_lastData.clear();
    rc = api->execute_action(ud, "clean", "{\"categories\":[\"logs\",\"pycache\"]}");
    CHECK(rc == 0, "clean rc==0");
    CHECK(countFiles(R + "\\Hermes\\data\\logs") == 0, "data\\logs 已清空");
    CHECK(!pathExists(R + "\\studio\\logs\\launcher.log"), "studio\\logs\\launcher.log 已删");
    CHECK(pathExists(R + "\\studio\\logs"), "logs 目录本身保留");
    CHECK(countFiles(R + "\\Hermes\\python\\pkg\\__pycache__") == 0 || !pathExists(R + "\\Hermes\\python\\pkg\\__pycache__"), "__pycache__ 已清");
    CHECK(pathExists(R + "\\studio\\cache\\downloads\\hermes-runtime.tar.gz"), "未勾选的 upd_dl 未动");
    CHECK(pathExists(R + "\\studio\\backups\\webui-pre-0.7.0.zip"), "未勾选的 backups 未动");
    CHECK(pathExists(R + "\\Hermes\\_home\\AppData\\Local\\npm-cache\\cacache.bin"), "未勾选的 pkgcache 未动");
    CHECK(pathExists(R + "\\WebViewData\\EBWebView\\Default\\Cache\\f_000001"), "未勾选的 webcache 未动");
    CHECK(pathExists(R + "\\WebViewData\\EBWebView\\Default\\Cookies\\db"), "Cookies (用户数据) 永远不动");
    CHECK(countFiles(R + "\\Hermes\\webui\\node_modules\\pkga\\logs") == logsBefore, "node_modules 内 logs 未被碰");
    CHECK(g_lastData.find("\"cleaned\"") != std::string::npos, "clean 输出 cleaned 数据");

    printf("== clean-safe (无风险一键) ==\n");
    g_lastData.clear();
    rc = api->execute_action(ud, "clean-safe", "{}");
    CHECK(rc == 0, "clean-safe rc==0");
    CHECK(!pathExists(R + "\\studio\\cache\\downloads\\hermes-runtime.tar.gz"), "无风险 upd_dl 已清");
    CHECK(pathExists(R + "\\studio\\cache\\downloads"), "downloads 目录重建保留");
    CHECK(!pathExists(R + "\\WebViewData\\EBWebView\\Default\\Cache\\f_000001"), "无风险 webcache 已清");
    CHECK(pathExists(R + "\\studio\\backups\\webui-pre-0.7.0.zip"), "高风险 backups 一键不清");
    CHECK(pathExists(R + "\\Hermes\\_home\\AppData\\Local\\npm-cache\\cacache.bin"), "低风险 pkgcache 一键不清");
    CHECK(!pathExists(R + "\\crash.dmp"), "无风险 *.dmp 已清");
    CHECK(!pathExists(R + "\\Hermes\\_home\\tmp\\x.tmp"), "无风险 temp 已清");

    api->destroy(ud);
    printf(g_fail ? "\nRESULT: FAIL (%d)\n" : "\nRESULT: PASS\n", g_fail);
    system(("rd /s /q \"" + base + "\" >nul 2>&1").c_str());
    return g_fail ? 1 : 0;
}
