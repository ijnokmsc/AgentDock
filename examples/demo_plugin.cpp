// 示例插件: 演示 C ABI 插件接口。编译为 demo_plugin.dll 后放入
// <portable>/data/studio/plugins/demo/plugin.dll
#include <cstdio>
#include <cstring>
#include <string>
#include "hs_plugin.h"

#ifdef _WIN32
#define EXPORT extern "C" __declspec(dllexport)
#else
#define EXPORT extern "C"
#endif

static const hs_host_api_vtable* g_host = nullptr;
static void* g_ud = nullptr;

static void get_info(hs_str* name, hs_str* ver, hs_str* desc) {
    if (name) *name = "demo-plugin";
    if (ver)  *ver  = "0.1.0";
    if (desc) *desc = "HermesStudio 示例插件 (演示 C ABI 接口)";
}

static hs_i32 create(const hs_host_api_vtable* host, void** ud) {
    g_host = host;
    g_ud = (void*)0x1234;
    if (ud) *ud = g_ud;
    if (host && host->log) {
        host->log(HS_LOG_INFO, "demo", "示例插件已加载");
    }
    return 0;
}

static void destroy(void* ud) {
    (void)ud;
    if (g_host && g_host->log) g_host->log(HS_LOG_INFO, "demo", "示例插件已卸载");
}

static hs_event_result on_event(void* ud, hs_event ev, hs_str payload) {
    (void)ud; (void)payload;
    switch (ev) {
        case HS_EVENT_APP_STARTING:
            if (g_host && g_host->log) g_host->log(HS_LOG_DEBUG, "demo", "APP_STARTING 事件");
            return HS_EVENT_OK;   // 不阻止启动
        default:
            return HS_EVENT_OK;
    }
}

static void get_ui_entries(void* ud, void (*sink)(const hs_ui_entry*, void*), void* ctx) {
    (void)ud;
    hs_ui_entry e;
    e.id = "demo.main";
    e.title = "示例插件";
    e.icon = "";
    e.json_schema = "";
    e.default_config = "";
    e.actions = "";
    if (sink) sink(&e, ctx);
}

static hs_i32 execute_action(void* ud, hs_str action_id, hs_str payload) {
    (void)ud; (void)payload;
    if (g_host && g_host->log) g_host->log(HS_LOG_INFO, "demo",
        std::string("动作: " + std::string(action_id)).c_str());
    return 0;
}

static const hs_plugin_api_vtable g_api = {
    HS_ABI_VERSION,
    get_info,
    create,
    destroy,
    on_event,
    get_ui_entries,
    execute_action,   // v1.1
};

EXPORT const hs_plugin_api_vtable* hs_plugin_entry(void) {
    return &g_api;
}
