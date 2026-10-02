# ADR-009: Agent 描述符注册表与构建元数据收敛

- 状态：Accepted (2026-09-29)
- 关联：ADR-007（launcher_v3 生命周期）、ADR-008（DSH 便携化）

## 背景

ADR-008 把定位从"Hermes 便携包管理器"扩展为"多 Agent 便携平台"，但接入
第二个 Agent (DSH) 时是 per-agent 复制粘贴：Runtime、Builder、Proc、settings、
Bridge 命令分支、HTML 卡片、原生页签七处各写一份。壳层唯一已泛化的是
`WebAgentProc` 基类与 `AgentWaitSpec` 等待页。接第三个 Agent 的边际成本
= 再抄七处，且品牌/文案/端口/探测方式散落各处容易漂移。

## 决策

1. **新增 `src/core/AgentSpec.{h,cpp}`**：`AgentDescriptor` 描述符 +
   `hs::agents::registry()` 静态注册表现役 Agent（hermes / dsh），字段：
   `id, displayName, buildTitle, buildNote, targetDirLabel, defaultPort,
   probeKind(PortReady | HttpWithToken), buildCmdPrefix, updateIds, hasBuilder`。
2. **本期只收敛"元数据消费"**：HTML 构建页卡片标题/说明/目标目录默认值、
   Bridge `agent.list` 下发，均从注册表取值（数据驱动）；页签/等待页已有
   泛化样板（MainWnd `AgentWaitSpec`），后续逐步切到描述符。
3. **不改进程管理与构建实现**：`HermesProc/DshProc/PortableBuilder/
   DshBuilder` 保持现状；既有 bridge 命令（`build.*` / `build.dsh.*`）与
   settings.json schema 不变，对外行为逐项等价。
4. **接第 4 个 Agent 的路径**（未来 ADR）：新增 XxxRuntime + WebAgentProc
   子类（~30 行）+ XxxBuilder（或挂入未来的通用 BuildPipeline 五段式）+
   registry 登记一条描述符 + HTML 构建页一张卡片。

## 后果

- 正：Agent 共享元数据（展示名/端口/探测方式/更新组件）有了唯一事实源；
  前端不再硬编码 Agent 文案；新 Agent 的接入清单明确为"描述符 + 七件套"。
- 负：注册表与既有硬编码分支（Bridge 命令前缀、页签数组）并存一个过渡期，
  两者不一致时以注册表为准逐步替换。
- 中立：settings.json 保持 per-agent struct（RuntimeConfig / DshSettings），
  泛化为 agents 数组留待真正需要运行时增删 Agent 时再做（便携场景静态
  编译期注册已够用）。

## 验收记录 (2026-09-29)

- `agent.list` 命令下发两条描述符，构建页标题/说明/目标目录按注册表填充；
- Hermes/DSH 构建→启动→停止→更新链路未触碰实现层，行为等价（冒烟通过）。
