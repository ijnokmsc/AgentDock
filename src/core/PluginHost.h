#pragma once
// 插件宿主: 扫描 <便携包根>/plugin/<id>/plugin.dll, 加载、校验 ABI、分发事件。
//
// 插件机制的目标是"通过插件加载扩展启动器"。提供:
//   - 生命周期钩子 (启动前/后/停止前)
//   - 体检项注册
//   - 声明式 UI (导航入口 + 配置面板)
//   - 模型 Provider 类型注册
//   - 更新源适配器 (钩子占位)
//
// 插件目录锚定便携包根 (启动器自有, 不进任何 Agent 子目录):
//   <portableRoot>/plugin/<id>/plugin.dll + plugin.json
//
// 插件启停状态: <portableRoot>/plugin/state.json
//   { "schema": 1, "plugins": { "<id>": { "enabled": true, "addedAt": "..." } } }
//   未记录 = 默认启用。停用的插件不加载。

#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <memory>
#include <map>

#include "Types.h"
#include "../../include/hs_plugin.h"

namespace fs = std::filesystem;

namespace hs {

// 插件元信息 (来自 plugin.json)
struct PluginInfo {
    std::string id;
    std::string name;
    std::string version;
    std::string description;
    std::string author;
    fs::path    dir;
    fs::path    dllPath;
    bool        enabled   = true;   // 用户开关 (来自 plugins-state.json); 停用 = 不加载
    bool        sidebarEnabled = true;  // 用户开关: 侧边栏入口显示 (插件仍加载运行)
    bool        quick     = true;   // HSPP v1.3: 是否出现在快捷区 (悬浮菜单/概览卡片), 清单可声明 false
    bool        loaded    = false;  // 本次进程内是否已加载 DLL
    std::string loadError;

    // 插件声明的 UI 入口 (get_ui_entries)
    struct UiEntry {
        std::string id;
        std::string title;
        std::string icon;
        std::string jsonSchema;
        std::string defaultConfig;
        std::string actions;   // JSON 数组: [{"id","label","confirm"}]
        bool sidebar = false;  // v1.2: 显示在启动器左侧边栏
        // v1.3 约定 (actions 元素可选字段, 不占 ABI):
        //   "type": "button"(默认) | "switch"  — switch 渲染为滑动开关,
        //   "default": bool                    — switch 初始状态 (无宿主记录时)
        //   "quick": false                     — 该动作不进快捷区 (可选)
    };
    int abiVersion = 0;        // 插件 DLL 的 ABI 版本 (1 = 无 sidebar 字段)
    std::vector<UiEntry> uiEntries;
};

class PluginHost {
public:
    PluginHost();
    ~PluginHost();

    // 扫描 plugins 目录, 找出所有候选 (不加载), 合并启停状态
    std::vector<PluginInfo> discover() const;

    // 加载并初始化所有**启用**的插件
    void loadAll();

    // 分发一个事件到所有已加载插件
    // 返回 true = 没有插件 veto; false = 有插件 veto (宿主应阻止启动等)
    bool dispatch(hs_event ev, const std::string& payload = {});

    // 查询已加载插件列表
    const std::vector<PluginInfo>& loadedPlugins() const { return loaded_; }

    // 查询全部插件 (含停用的) 及状态
    std::vector<PluginInfo> allPlugins() const;
    // 插件管理操作 (改完状态后需 reload)
    bool setEnabled(const std::string& id, bool enabled, std::string* err = nullptr);
    bool removePlugin(const std::string& id, std::string* err = nullptr);
    // 侧边栏入口用户开关 (仅影响显示, 插件保持加载; 无需 reload)
    bool setSidebar(const std::string& id, bool visible, std::string* err = nullptr);

    // 重扫 + 重载 (改启停后调用)
    void reload();

    // 获取某个已加载插件的 UI 入口
    const std::vector<PluginInfo::UiEntry>* uiEntriesOf(const std::string& id) const;

    // 调用插件动作 (用户在插件面板点击按钮时)。返回 0=成功。
    hs_i32 executeAction(const std::string& pluginId, const std::string& actionId,
                         const std::string& payload);

    // 执行结果升级版: 除 rc 外带回执行期间该插件经 hs_log 输出的尾部日志
    // (结果反馈基建: 悬浮菜单/概览卡片/插件页三处共用)。
    struct ActionOutcome {
        hs_i32      rc = 0;
        std::string logTail;      // 多行, 已裁剪 (最多 8 行/600 字节)
        std::string data;         // v1.4: 插件经 "@@DATA@@{json}" 日志行回传的结构化结果
                                  // (如清理插件的扫描清单), 空 = 本次无; 不进 logTail
    };
    ActionOutcome executeActionEx(const std::string& pluginId, const std::string& actionId,
                                  const std::string& payload);

    // HSPP v1.3 switch 动作的宿主侧状态 (持久化 plugins-state.json)。
    // declaredDefault = 动作声明的 "default" (无宿主记录时用)。
    bool switchState(const std::string& pluginId, const std::string& actionId,
                     bool declaredDefault) const;
    void setSwitchState(const std::string& pluginId, const std::string& actionId, bool v);
    // 全量快照 (推送给 HTML): {pluginId: {actionId: state}}
    std::map<std::string, std::map<std::string, bool>> allSwitchStates() const;

    // 从 zip 安装插件: 解包 -> 校验清单 (id==目录名, api==1) -> 落位 plugin/<id>/
    // -> 自动重载。zip 内须含 plugin.json (根目录或单层子目录)。
    bool installFromZip(const fs::path& zipPath, std::string* err = nullptr);

private:
    bool loadOne(PluginInfo& info);
    void unloadAll();
    void loadState();    // 读 plugins-state.json
    void saveState();    // 写 plugins-state.json

    std::vector<PluginInfo> loaded_;         // 已加载的启用插件
    std::vector<PluginInfo> discovered_;     // 扫描出的全部插件 (含停用)
    std::vector<PluginInfo> attempted_;      // 本次进程的加载尝试 (含失败, 带 loadError/loaded/uiEntries)
    // 每个加载插件的原生句柄
    struct PluginInstance;
    std::vector<std::unique_ptr<PluginInstance>> instances_;

    // 启停状态持久化: <id> -> enabled
    std::map<std::string, bool> state_;
    // 侧边栏显示偏好: <id> -> visible (缺省 true)
    std::map<std::string, bool> sidebarState_;
    // HSPP v1.3 switch 动作状态: <id> -> <actionId> -> state (缺省用声明 default)
    std::map<std::string, std::map<std::string, bool>> switchState_;
};

} // namespace hs
