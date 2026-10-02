#include "ProxyTester.h"

#include <chrono>

namespace hs::net {

ProxyTestResult testProxy(const ProxyConfig& cfg, int timeoutSec) {
    ProxyTestResult r;
    HttpOptions opt;
    opt.proxy = cfg;
    opt.timeoutSec = timeoutSec;
    opt.userAgent = "AgentDock-ProxyTest/1.0";

    // 探测端点: 轻量、无副作用; 多个候选提高弱网/镜像可用性
    const char* endpoints[] = {
        "https://registry.npmmirror.com/-/ping",
        "https://www.baidu.com/favicon.ico",
        "https://nodejs.org/dist/index.json",
    };
    for (const char* url : endpoints) {
        auto t0 = std::chrono::steady_clock::now();
        auto resp = httpGet(url, opt);
        auto ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0).count();
        if (resp.ok && resp.status > 0) {
            r.ok = true;
            r.latencyMs = ms;
            switch (cfg.mode) {
            case ProxyConfig::Mode::System:  r.detail = "经系统代理访问 " + std::string(url) + " 成功"; break;
            case ProxyConfig::Mode::Manual:  r.detail = "经 " + cfg.host + ":" + std::to_string(cfg.port) + " 访问 " + std::string(url) + " 成功"; break;
            default:                         r.detail = "直连访问 " + std::string(url) + " 成功"; break;
            }
            return r;
        }
        r.detail = resp.error.empty() ? ("HTTP " + std::to_string(resp.status)) : resp.error;
    }
    return r;
}

} // namespace hs::net
