// AgentDock 插件 ABI (C ABI, 纯 C 导出; 符号 hs_* 为协议约定, 与品牌名无关)。
//
// 插件形态: 一个目录 plugins/<plugin-id>/ 下放置 plugin.dll + plugin.json (清单)。
// plugin.dll 导出 C 函数表 (HS_PLUGIN_API), 宿主按 ABI 版本校验后加载。
//
// 设计约束:
//   - 纯 C 接口 (extern "C"), 不暴露任何 C++ 类型/Qt 类型, ABI 稳定
//   - 宿主提供 hs_host_api_vtable 结构体, 插件通过它调用宿主能力
//   - 插件崩溃由宿主在独立进程中隔离? —— 不, 当前 M0 阶段允许插件在宿主进程内,
//     但文档标注此风险, 未来可升级为进程外托管。故插件作者应避免内存错误。
//
// 生命周期:
//   hs_plugin_get_abi      -> 宿主校验 ABI 版本与 API 版本
//   hs_plugin_create       -> 宿主传入 host api, 插件返回自己的插件句柄
//   hs_plugin_destroy      -> 卸载前清理
//   hs_plugin_on_event     -> 宿主广播生命周期/事件 (启动前/启动后/体检/更新...)
//
// ABI 演进: 每个字段只增不减, 结构体末尾追加; 通过 API_VERSION 兼容检查。

#ifndef HS_PLUGIN_H
#define HS_PLUGIN_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HS_ABI_VERSION       2      // 插件目录格式 / 导出函数表版本 (v2: ui_entry.sidebar)
#define HS_HOST_API_VERSION  1      // 宿主 API 表版本

/* ------------------------------------------------------------------ */
/* 基础类型                                                             */
/* ------------------------------------------------------------------ */
typedef uint32_t hs_u32;
typedef uint64_t hs_u64;
typedef int32_t  hs_i32;

/* 字符串: 宿主保证在调用期间有效, 插件不应长期持有指针 (需拷贝) */
typedef const char* hs_str;

/* 日志级别 */
typedef enum hs_log_level {
    HS_LOG_DEBUG = 0,
    HS_LOG_INFO  = 1,
    HS_LOG_WARN  = 2,
    HS_LOG_ERROR = 3,
} hs_log_level;

/* 宿主事件类型 (on_event 的 event 参数) */
typedef enum hs_event {
    HS_EVENT_APP_STARTING   = 1,   // 启动 Hermes 前 (可 veto: 返回非 0 阻止启动)
    HS_EVENT_APP_STARTED    = 2,   // Hermes 启动后
    HS_EVENT_APP_STOPPING   = 3,   // Hermes 停止前
    HS_EVENT_PREFLIGHT      = 4,   // 体检阶段 (可注册检查项)
    HS_EVENT_UPDATE_CHECK   = 5,   // 更新检查 (可注册更新源适配器)
    HS_EVENT_SETTINGS_SAVED = 6,   // 设置保存后
} hs_event;

/* 事件返回码 */
typedef enum hs_event_result {
    HS_EVENT_OK        = 0,
    HS_EVENT_VETO      = 1,   // 阻止 (如阻止启动)
    HS_EVENT_ASYNC     = 2,   // 已接管异步, 宿主不应等待
} hs_event_result;

/* ------------------------------------------------------------------ */
/* 声明式 UI 描述 (受限 UI: 插件不拿裸 QWidget, 只描述, 宿主渲染)          */
/* ------------------------------------------------------------------ */

/* 插件在导航栏挂一个入口 + 一个配置面板 */
typedef struct hs_ui_entry {
    hs_str id;              // 唯一 id, 如 "myplugin.main"
    hs_str title;           // 导航标题
    hs_str icon;            // 图标名 (宿主内置图标集), 可空
    hs_str json_schema;     // 配置项的 JSON Schema (宿主渲染表单), 可空
    hs_str default_config;  // 默认配置 JSON, 可空
    hs_str actions;         /* v1.1: JSON 数组描述动作按钮, 如
                               [{"id":"clear","label":"清空日志","confirm":"确认清空?"}]
                               可空 = 无动作按钮 */
    hs_i32 sidebar;         /* v1.2: 非 0 = 该入口显示在启动器左侧边栏;
                               0 = 仅显示在插件管理卡片内。
                               v1 ABI 插件无此字段, 视为 0 */
} hs_ui_entry;

/* ------------------------------------------------------------------ */
/* 宿主 API 表 (vtable)                                                 */
/* ------------------------------------------------------------------ */
struct hs_host_api_vtable;

/* 插件导出的函数表 */
typedef struct hs_plugin_api_vtable {
    hs_u32 abi_version;     /* 必须 == HS_ABI_VERSION */

    /* 插件信息 (宿主在 create 前可调用 get_info 获取) */
    void (*get_info)(hs_str* name, hs_str* version, hs_str* description);

    /* 创建插件实例。host 是宿主 vtable, user_data 由插件自己管理。
       返回 0 表示成功。 */
    hs_i32 (*create)(const struct hs_host_api_vtable* host, void** user_data);

    /* 销毁插件实例 (卸载前调用) */
    void (*destroy)(void* user_data);

    /* 事件分发。返回 HS_EVENT_* */
    hs_event_result (*on_event)(void* user_data, hs_event ev, hs_str payload_json);

    /* 声明式 UI 入口 (可返回多个; host 提供注册回调) */
    void (*get_ui_entries)(void* user_data,
                           void (*sink)(const hs_ui_entry*, void* ctx),
                           void* ctx);

    /* v1.1: 执行插件声明的动作 (用户在插件面板点击按钮时宿主调用)。
       返回 0=成功。插件可在这里执行清空日志等操作。 */
    hs_i32 (*execute_action)(void* user_data, hs_str action_id, hs_str payload_json);
} hs_plugin_api_vtable;

/* 宿主 API vtable —— 插件能调用的宿主能力 */
typedef struct hs_host_api_vtable {
    hs_u32 api_version;   /* 必须 == HS_HOST_API_VERSION */

    /* 日志 */
    void (*log)(hs_log_level lvl, hs_str tag, hs_str msg);

    /* 读写启动器自己的配置 (JSON 字符串) */
    hs_i32 (*config_get)(hs_str key, hs_str* out_json);        // 返回 0=成功; 调用方需释放
    hs_i32 (*config_set)(hs_str key, hs_str json_value);
    void  (*string_free)(hs_str s);                            // 释放 config_get 返回的字符串

    /* 向体检表注册一个自定义检查项 (HS_EVENT_PREFLIGHT 时也可) */
    void (*register_check)(hs_str id, hs_str title,
                           hs_i32 level, /* 0 pass 1 warn 2 fail */
                           hs_str detail);

    /* 注册一个自定义模型 Provider 类型 */
    void (*register_provider_type)(hs_str kind, hs_str display_name);

    /* 查询 / 修改模型配置 */
    hs_i32 (*models_get)(hs_str* out_json);    // 返回整个 models.json 的 JSON
    hs_i32 (*models_set)(hs_str json);

    /* 进度回调 (用于更新/长任务) */
    void (*progress)(hs_str task, hs_i32 percent, hs_str status);
} hs_host_api_vtable;

/* ------------------------------------------------------------------ */
/* 导出入口 (插件 dll 必须导出)                                          */
/* ------------------------------------------------------------------ */
/*
   extern "C" __declspec(dllexport) const hs_plugin_api_vtable* hs_plugin_entry(void);
   返回指向插件函数表的指针。宿主调用 hs_plugin_entry() 获取表。
*/

#ifdef __cplusplus
}
#endif

#endif /* HS_PLUGIN_H */
