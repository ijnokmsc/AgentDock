// 端到端: 在真实便携包内跑一遍完整启动/停止链路。
//
//   1. Paths::autoDetect()  —— 验证 exe 目录锚点 + 布局指纹
//   2. Launcher::start(WebUI, port)
//        - 注入沙箱环境 (_home)
//        - node webui/dist/server/index.js start <port>
//        - web-ui 通过 HERMES_BIN 自动拉起 agent bridge
//   3. 轮询 http://127.0.0.1:<port>/ 直到有响应 (或超时)
//   4. 顺带观察 agent bridge 端口
//   5. Launcher::stop() —— 端口应释放、pid 文件应清理
//
// 用法: e2e_test [port] [waitSec]
//   port    默认 8648
//   waitSec 默认 120
//
// 注意: 必须在便携包根目录运行 (与 HermesStudio.exe 同级), 否则 autoDetect
//       会锚到别处。测试自身不应写任何包外文件, 由外部脚本比对快照。
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <string>
#include <thread>

#include "app/Paths.h"
#include "core/Launcher.h"
#include "platform/HttpClient.h"
#include "platform/PortScanner.h"

using namespace hs;

static void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

int main(int argc, char** argv) {
    const int port    = (argc > 1) ? std::atoi(argv[1]) : 8648;
    const int waitSec = (argc > 2) ? std::atoi(argv[2]) : 120;

    printf("=== 1) autoDetect ===\n");
    const bool okRoot = Paths::autoDetect();
    printf("  autoDetect = %s\n", okRoot ? "true" : "false");
    printf("  root       = %s\n", Paths::root().string().c_str());
    printf("  layout     = %s\n",
           Paths::isOfficialLayout() ? "OFFICIAL"
                                     : (Paths::isLegacyLayout() ? "LEGACY" : "UNKNOWN"));
    printf("  venvPython = %s\n", Paths::venvPython().string().c_str());
    printf("  nodeExe    = %s\n", Paths::nodeExe().string().c_str());
    printf("  webUiEntry = %s\n", Paths::webUiEntry().string().c_str());
    printf("  sandbox    = %s\n", Paths::sandboxDir().string().c_str());
    if (!okRoot) {
        printf("[FAIL] autoDetect 未通过\n");
        return 1;
    }

    // 前置: 端口不能已被占用 (否则等于在测别人)
    if (auto pre = net::pidByPort(port)) {
        printf("[FAIL] 端口 %d 已被 pid %lu (%s) 占用, 先停掉再测\n",
               port, *pre, net::processName(*pre).c_str());
        return 1;
    }

    printf("\n=== 2) start(WebUI, %d) ===\n", port);
    Launcher lc;
    auto sr = lc.start(StartMode::WebUI, port);
    printf("  ok      = %s\n", sr.ok ? "true" : "false");
    printf("  message = %s\n", sr.message.c_str());
    if (!sr.ok) {
        printf("[FAIL] 启动失败\n");
        return 2;
    }

    printf("\n=== 3) 等待 web-ui 就绪 (最多 %d s) ===\n", waitSec);
    net::HttpOptions ho;
    ho.timeoutSec = 3;
    ho.proxy.mode = net::ProxyConfig::Mode::Direct;   // 本地回环不走代理

    const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/";
    bool up = false;
    long lastStatus = 0;
    size_t lastBody = 0;
    for (int i = 0; i < waitSec; ++i) {
        auto resp = net::httpGet(url, ho);
        if (resp.ok) {
            up = true;
            lastStatus = resp.status;
            lastBody   = resp.body.size();
            printf("  [%3ds] HTTP %ld, body=%zu B  <-- 就绪\n", i, resp.status, resp.body.size());
            break;
        }
        if (i % 5 == 0) {
            auto p = net::pidByPort(port);
            printf("  [%3ds] 尚未就绪 (listener pid=%s)\n", i,
                   p ? std::to_string(*p).c_str() : "-");
        }
        sleepMs(1000);
    }

    // agent bridge 是 web-ui 拉起来的, 给一点额外时间
    sleepMs(2000);
    auto webPid = net::pidByPort(port);
    auto brPid  = net::pidByPort(18765);
    printf("\n  web-ui(:%d) pid = %s\n", port,
           webPid ? std::to_string(*webPid).c_str() : "(无)");
    printf("  agent-bridge(:18765) pid = %s\n",
           brPid ? std::to_string(*brPid).c_str() : "(无)");

    printf("\n=== 4) stop() ===\n");
    const bool stopped = lc.stop();
    printf("  stop() = %s\n", stopped ? "true" : "false");
    sleepMs(2500);

    auto webAfter = net::pidByPort(port);
    auto brAfter  = net::pidByPort(18765);
    printf("  web-ui(:%d) 端口释放 = %s\n", port, webAfter ? "NO" : "YES");
    printf("  agent-bridge(:18765) 端口释放 = %s\n", brAfter ? "NO" : "YES");

    // ---------------------------------------------------------------
    // 判定
    // ---------------------------------------------------------------
    printf("\n=== 结论 ===\n");
    if (!up) {
        printf("[FAIL] web-ui 在 %d s 内没有响应 (%s)\n", waitSec, url.c_str());
        return 3;
    }
    printf("[PASS] web-ui 就绪: HTTP %ld, %zu B\n", lastStatus, lastBody);

    if (!webAfter) printf("[PASS] web-ui 已停止\n");
    else           printf("[WARN] web-ui 仍在监听 :%d (pid %lu)\n", port, *webAfter);

    if (!brAfter) printf("[PASS] agent bridge 已停止\n");
    else          printf("[WARN] agent bridge 仍在监听 :18765 (pid %lu)\n", *brAfter);

    if (webAfter || brAfter) return 4;
    return 0;
}
