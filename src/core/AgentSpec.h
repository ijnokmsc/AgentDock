// AgentSpec.h —— Agent 描述符注册表 (ADR-009)
//
// 把"Agent"从 per-agent 复制粘贴收敛为一条描述符: 壳层 (页签/等待页/健康探测/
// 构建命令前缀/更新中心组件行) 按描述符取值, 接入新 Agent 只需:
//   1. 新增一份 XxxRuntime + WebAgentProc 子类 (壳层进程管理已泛化)
//   2. 新增一份 XxxBuilder (或挂入 BuildPipeline 五段式)
//   3. 在 registry() 里登记一条描述符 + HTML 构建页一张卡片
// 本期 (ADR-009) Hermes/DSH 迁移到描述符驱动, 对外行为逐项等价。
#pragma once
#include <string>
#include <vector>

namespace hs::agents {

// 健康探测形态: 端口监听即就绪 (Hermes) / 需 HTTP 探测 + token 横幅 (DSH)
enum class ProbeKind { PortReady, HttpWithToken };

struct AgentDescriptor {
    const char* id;              // "hermes" / "dsh" (bridge 命令前缀 / agent.list 的 key)
    const char* displayName;     // 用户可见名 ("Hermes" / "DeepSeek Harness")
    const char* buildTitle;      // 构建页卡片标题
    const char* buildNote;       // 构建页卡片说明 (含默认目标目录)
    const char* targetDirLabel;  // 构建目标目录占位/默认值 (""=Hermes 锚根自身)
    int         defaultPort;     // Web UI 默认端口 (0=由内核自定)
    ProbeKind   probeKind;
    const char* buildCmdPrefix;  // 既有 bridge 构建命令前缀: "build" / "build.dsh"
    const char* updateIds;       // 更新中心组件 id (csv): "kernel,webui" / "dsh,dshnode"
    bool        hasBuilder;      // 是否支持从零构建便携包
};

// 静态注册表: 现役 Agent 一条不缺; 顺序即构建页/页签展示顺序
const std::vector<AgentDescriptor>& registry();

// 按 id 查找 (未找到返回 nullptr)
const AgentDescriptor* find(const std::string& id);

}  // namespace hs::agents
