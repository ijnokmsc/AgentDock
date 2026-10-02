// HSPP v1.3 开关动作测试插件: 验证 type:"switch" 声明 + payload {"value":bool} + hs_log 结果回传。
// 编译: cl /LD /I<hs_plugin.h 目录> switch_test_plugin.cpp /Fe:plugin.dll /link /EXPORT:hs_plugin_entry
#include <string>
#include <windows.h>
#include "hs_plugin.h"

static const hs_host_api_vtable* g_host = nullptr;
static void* g_ud = (void*)0xC0DE;

static hs_i32 hsCreate(const hs_host_api_vtable* host, void** ud) {
    g_host = host;
    *ud = g_ud;
    return 0;
}
static void hsDestroy(void*) {}

static void hsGetUiEntries(void*, void (*sink)(const hs_ui_entry*, void*), void* ctx) {
    static const char* kActionsArr =
        "[{\"id\":\"auto_clean\",\"label\":\"自动清理\",\"type\":\"switch\",\"default\":true}]";
    static hs_ui_entry e = {};
    e.id = "sw.main";
    e.title = "开关测试";
    e.icon = "";
    e.json_schema = "";
    e.default_config = "";
    e.actions = kActionsArr;
    e.sidebar = 1;
    sink(&e, ctx);
}

static hs_i32 hsExecute(void*, const char* action_id, const char* payload_json) {
    if (!g_host || !g_host->log) return -1;
    if (!action_id || std::string(action_id) != "auto_clean") return -2;   // 未知动作报错
    std::string msg = std::string("switch 动作收到: action=") + action_id +
                      " payload=" + (payload_json ? payload_json : "(null)");
    g_host->log(HS_LOG_INFO, "switch_test", msg.c_str());
    // 模拟耗时后回报结果
    g_host->log(HS_LOG_INFO, "switch_test", "应用完成");
    return 0;
}

static hs_event_result hsOnEvent(void*, hs_event, const char*) { return HS_EVENT_OK; }
static void hsGetInfo(hs_str* name, hs_str* version, hs_str* description) {
    if (name) *name = "switch_test";
    if (version) *version = "1.0.0";
    if (description) *description = "HSPP v1.3 switch 动作测试";
}

static hs_plugin_api_vtable g_vtable = {
    HS_ABI_VERSION,
    hsGetInfo, hsCreate, hsDestroy, hsOnEvent, hsGetUiEntries, hsExecute,
};

extern "C" __declspec(dllexport) const hs_plugin_api_vtable* hs_plugin_entry() {
    return &g_vtable;
}
