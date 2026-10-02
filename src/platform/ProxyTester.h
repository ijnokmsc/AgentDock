#pragma once
// 代理连通性测试 (设置页「测试代理」按钮): 用给定代理配置请求一个轻量
// 端点, 返回可达性与延迟。System 模式探测 IE 代理配置后按 Manual 走。
#include <string>
#include <optional>

#include "HttpClient.h"

namespace hs::net {

struct ProxyTestResult {
    bool        ok = false;
    int         latencyMs = -1;    // -1 = 不可用
    std::string detail;            // 成功: 实际生效的代理/直连说明; 失败: 原因
};

ProxyTestResult testProxy(const ProxyConfig& cfg, int timeoutSec = 8);

} // namespace hs::net
