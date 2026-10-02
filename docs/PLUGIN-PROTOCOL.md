# AgentDock 插件协议规范（HSPP v1.4）

> HSPP = AgentDock Plugin Protocol（历史上叫 HermesStudio Plugin Protocol，
> C ABI 符号 `hs_*` 为协议约定，与品牌名无关）。本规范约束**原生插件**
> （C ABI DLL）的目录结构、清单格式、ABI 版本策略与行为契约；
> 宿主实现 = `src/core/PluginHost.cpp`，ABI 头 = `include/hs_plugin.h`。
> DSH 插件（npm/pnpm 形态）是另一套协议，见本文末尾"与 DSH 插件的关系"。
>
> **v1.3 增量（纯宿主侧解析约定，不改 C ABI，老插件零改动）**：
> actions 元素新增 `type`/`default`/`quick` 可选字段（§4.1）；
> 清单新增 `quick` 可选字段（§1.1）；动作执行回传插件日志尾部（§4.2）；
> zip 一键安装（§8）。
>
> **v1.4 增量（仍为纯宿主侧约定，不改 C ABI）**：动作执行期间插件可用
> `@@DATA@@{json}` 日志行回传**结构化结果**，宿主抽出后随
> `plugin.action.result` 事件的 `data` 字段给 UI，不占用日志尾部（§4.3）；
> actions 元素新增 `arg` 可选字段（`"checklist"`：UI 把勾选项经 payload 回传）。

## 1. 目录与清单

```
<便携包根>/plugin/<plugin-id>/
├── plugin.dll        # 必需; 导出 hs_plugin_entry()
└── plugin.json       # 必需; 清单 (UTF-8, 无 BOM)
```

> 插件目录锚定**便携包根**（HermesStudio.exe 所在目录），不属于任何 Agent
> 子目录（Hermes\/DSH\/...）——插件是启动器的扩展，Agent 目录被更新或重建
> 时不应受影响。

### 1.1 plugin.json 清单规范

| 字段 | 类型 | 必需 | 说明 |
|---|---|---|---|
| `id` | string | **是** | 插件唯一 id; **必须与目录名 `<plugin-id>` 完全一致**; `[a-z0-9-]+` |
| `name` | string | **是** | 显示名 |
| `version` | string | **是** | 语义化版本 `x.y.z[-suffix]` |
| `api` | number | **是** | 宿主 API 版本, 当前 `1`; 高于宿主支持版本 → 拒载并标错误 |
| `description` | string | 否 | 一句话描述 |
| `author` | string | 否 | 作者 |
| `minHostVersion` | string | 否 | 宿主最低版本要求 (信息展示, 不强制) |
| `quick` | bool | 否 | v1.3: 是否出现在快捷区（悬浮菜单/概览页插件快捷卡片），缺省 `true`；`false` = 仅在插件管理页可见 |

校验规则（宿主 discover 时执行，违规不加载并在列表显示 `loadError`）：
- 缺 `id`/`name`/`version`/`api` 任一 → `manifest 缺少字段 <field>`
- `id` 与目录名不一致 → `manifest id 与目录名不一致`
- `api` 为非数字或 > 宿主支持版本 → `manifest api 版本不受支持`
- `plugin.dll` 缺失 → 目录整体忽略（不算插件候选）

## 2. ABI 与版本策略

- `HS_ABI_VERSION`（插件函数表版本）当前为 **2**；`HS_HOST_API_VERSION`
  （宿主能力表版本）= **1**。
- **兼容判定（三级）**：
  1. 清单 `plugin.json` 的 `api` 字段必须 `== 1`（即
     `HS_HOST_API_VERSION`）—— discover 阶段校验，不符拒载；
  2. DLL 导出表 `abi_version` 必须在 `1..HS_ABI_VERSION` 区间 ——
     loadOne 校验，超出拒载；
  3. `abi_version >= 2` 的插件，宿主读取 `hs_ui_entry.sidebar` 字段；
     `abi_version == 1` 的旧插件结构体里没有该字段，宿主按 0 处理
     （不越界读）。
- **只增不减**：结构体字段只在末尾追加；新增枚举值不改变既有值；
  行为变更走新增字段/新增导出。
- v1.1 增量：`hs_ui_entry.actions` 字段 + `execute_action` 导出。
- v1.2 增量（ABI 2）：`hs_ui_entry.sidebar` 字段（见 §4）。

## 3. 生命周期

```
宿主启动 → discover() → (启用且校验通过) → LoadLibrary → hs_plugin_entry()
  → create(host_api, &user_data) → [运行期: on_event 广播 / get_ui_entries / execute_action]
宿主退出/停用/删除 → destroy(user_data) → FreeLibrary
```

- `on_event` 事件表：`APP_STARTING`(可 veto)、`APP_STARTED`、`APP_STOPPING`、
  `PREFLIGHT`、`UPDATE_CHECK`、`SETTINGS_SAVED`。
- veto 契约：`APP_STARTING` 返回 `HS_EVENT_VETO` 阻止本次启动；宿主必须把
  veto 结果反馈给用户。
- 宿主保证 `create` 之前先探测 `get_info`；`destroy` 与 `create` 一一配对。

## 4. 动作与侧边栏 (actions / sidebar)

`hs_ui_entry.actions` 为 JSON 数组字符串：

```json
[{"id": "clear", "label": "清空日志", "confirm": "确认清空?"}]
```

- `id`/`label` 必需；`confirm` 可选（宿主在执行前弹确认）。
- 用户点击 → 宿主调 `execute_action(user_data, action_id, payload_json)`；
  返回非 0 → 宿主提示失败（含 rc）。
- 动作必须幂等或自带确认；宿主不保证动作在哪个线程执行。

### 4.1 快捷区控件类型 (v1.3 约定, 不占 ABI)

actions 元素的可选字段（宿主解析，老插件未声明时全部走默认值）：

| 字段 | 取值 | 说明 |
|---|---|---|
| `type` | `"button"`(缺省) / `"switch"` | `switch` 渲染为滑动开关（悬浮菜单 ◉/○ 行、概览页 CSS 开关），点击翻转状态并回传 |
| `default` | bool | `switch` 的初始状态（宿主无记录时）；宿主实际状态持久化在 `plugin/state.json` 的 `switches` 段 |
| `quick` | bool(缺省 true) | `false` = 该动作不进快捷区（悬浮菜单/概览卡片），仅插件管理页内可见 |

**switch 执行契约**：用户翻转开关 → 宿主先更新并持久化宿主侧状态 → 以
`payload_json = {"value": <新状态>}` 调 `execute_action`。插件应把 payload
当作"用户期望的目标状态"应用。示例：

```json
[{"id": "auto_clean", "label": "自动清理", "type": "switch", "default": true}]
```

### 4.2 执行结果反馈 (v1.3)

插件在 `execute_action` 期间经宿主能力表 `log(lvl, tag, msg)` 输出的日志，
宿主按插件归属收集（环形缓冲，最近 200 条），并在动作结束后把**尾部
（≤8 行 / ≤600 字节）**随结果事件回传给 UI：悬浮菜单底部结果行、概览页
快捷卡片结果区、插件管理页共用。建议插件用日志输出执行明细（如清理了
多少文件），用户无需翻日志页即可看到结果。

### 4.3 结构化结果与清单回传 (v1.4, 纯约定不占 ABI)

`execute_action` 期间，插件输出**单行**日志
`@@DATA@@{...json...}`（标记与 JSON 间无空格要求，宿主取标记后的原文），
宿主把该行从日志尾部剔除，将 JSON 抽出放进 `plugin.action.result` 事件的
`data` 字段（悬浮菜单的人读结果行只显示普通日志尾部，不受影响）。
同一动作多次输出取最后一行；建议 ≤8KB。

**checklist 约定**（勾选列表, 参考实现 = 体积优化插件 clear-logs）：
`data.checklist` 结构：

```json
{"checklist": {"root": "I:\\PortableAgent", "scannedFiles": 131677,
  "scannedBytes": 5605387246, "durMs": 2281,
  "items": [{"id": "logs", "name": "日志文件", "risk": 0,
             "bytes": 2925873, "files": 89, "note": "运行日志…",
             "targets": ["Hermes\\data\\logs", "studio\\logs"]}]}}
```

- `risk`：0=无风险 / 1=低风险 / 2=高风险；UI 按 `risk==0` **默认勾选**，
  用户可改选（"仅无风险/全选/全不选"快捷链）。
- 空类别（`files==0`）不上列表；`targets` 为相对 root 的样例路径（建议 ≤4 条）。
- 接收勾选的动作在 actions 元素声明 `"arg":"checklist"`：UI 把勾选的
  `id` 数组经 `payload` 回传：`{"categories":["logs","pycache"]}`。
- 动作回传 `data.cleaned` 时 UI 自动重扫刷新清单（先扫后清的插件由此闭环）。
- 适合清单交互的动作声明 `"quick":false`（不进悬浮菜单/概览快捷卡——
  那里只有结果行没有列表）；"一键清理默认无风险项" 的动作保持 quick。

```json
[{"id":"clean-safe","label":"一键清理 (无风险)","confirm":"…"},
 {"id":"scan","label":"扫描垃圾文件","quick":false},
 {"id":"clean","label":"清理勾选项","quick":false,"arg":"checklist","confirm":"…"}]
```

### sidebar 字段 (v1.2, ABI 2)

`hs_ui_entry.sidebar` 非 0 时，宿主把该入口挂到**启动器左侧边栏**：

- 边栏显示 `title`；点击切到该入口的独立面板；
- 面板内渲染 `actions` 全部按钮（点击经 `execute_action`，带 confirm）；
- 插件需已加载（`loaded`）才会挂边栏；停用/加载失败的插件自动消失；
- 插件启停/删除/重扫后边栏随之重建；
- **用户偏好**：宿主为每个插件持久化侧边栏显示开关（`plugin.state.json`
  的 `sidebar` 字段，缺省开）。关闭后入口从边栏隐藏，但插件仍加载运行；
- `abi_version == 1` 的旧插件没有该字段，宿主按 0 处理（仅显示在
  插件管理卡片内）。

## 5. 宿主能力表 (host api v1)

| 能力 | 函数 | 备注 |
|---|---|---|
| 日志 | `log(lvl, tag, msg)` | 写入启动器日志 |
| 配置 | `config_get/set` | JSON 字符串；`hermesRoot` 等键由宿主定义 |
| 体检 | `register_check` | PREFLIGHT 事件期注册 |
| Provider | `register_provider_type` | 模型类型注册 |
| 模型配置 | `models_get/set` | 整个 models.json 读写 |
| 进度 | `progress(task, percent, status)` | 长任务反馈 |
| 内存 | `string_free` | 释放宿主返回的字符串 |

## 6. 与 DSH 插件的关系

| | 原生插件 (HSPP) | DSH 插件 |
|---|---|---|
| 形态 | C ABI DLL + plugin.json | npm 包 (`dsh plugin add`) |
| 作用面 | 启动器自身（UI/体检/事件/配置） | DSH Web UI（会话/工具/主题） |
| 管理 | 启动器插件页 (启停/删除) | 启动器插件页 DSH 卡 (装/卸/重启) |
| 协议 | 本规范 | dsh 官方 profile/bundle 机制 |

两套协议相互独立、不互转。启动器插件页分两个卡片展示，各自遵循各自协议。

## 8. zip 一键安装 (v1.3)

启动器插件页「安装插件 (zip)」→ 原生文件选择框 → 宿主执行：

1. 解压 zip 到 `plugin/_incoming-<pid>` 临时目录（miniz，失败即清理报错）；
2. 定位清单：zip 根目录的 `plugin.json`，或**唯一**含它的单层子目录；
3. 校验清单与 discover 同规则（`name/version/api` 必填、`api == 1`、
   `id` 与目录名一致且不含路径分隔符）；
4. 落位到 `plugin/<id>/`（同卷 rename，跨卷兜底 copy+remove；`<id>` 已存在
   则拒绝，需先删除旧版）；
5. 自动 `reload()` 并推送插件列表；安装结果（成功/失败原因）toast 反馈。

发布插件 = 把 `plugin/<id>/` 下的 `plugin.dll + plugin.json`（可选其余文件）
打成 zip。整包 zip 与单层目录 zip 均可。

## 7. 插件作者清单 (checklist)

1. 目录名 = `plugin.json` 的 `id`，全小写连字符
2. `plugin.dll` 导出 `hs_plugin_entry`，`abi_version = HS_ABI_VERSION`
3. `create/destroy` 配对，`create` 里不执行耗时操作
4. `on_event` 里非阻塞（veto 场景除外），长任务自行开线程
5. 提供 `plugin.json`（含 `api: 1`），UTF-8 无 BOM
6. 避免内存错误（当前为进程内加载，插件崩溃会拖垮启动器）
7. 动作默认进快捷区（悬浮菜单 + 概览页插件快捷卡片）；不适合一键执行
   的动作在 actions 元素声明 `"quick": false`；整个插件不想进快捷区则
   清单声明 `"quick": false`
8. `execute_action` 里用宿主 `log()` 输出执行明细（数量统计/结果 URL 等）
   —— 宿主会把日志尾部随动作结果回传到 UI
