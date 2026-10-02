// 启动链路测试: 用便携包原生链路启动 web-ui, 验证 Hermes agent bridge 被拉起、
// state() 能检测到 Running。
#include <cstdio>
#include <thread>
#include <chrono>
#include "core/Launcher.h"
#include "core/Types.h"
#include "app/Paths.h"
#include "platform/PortScanner.h"

using namespace hs;

int main() {
    if (!Paths::setRoot("I:/HermesPortable")) { printf("[FAIL] setRoot\n"); return 1; }

    Launcher launcher;
    printf("startWebUi 可用: %d\n", (int)launcher.modeAvailable(StartMode::WebUI));
    auto res = launcher.start(StartMode::WebUI, 8648);
    printf("start: ok=%d msg=%s\n", (int)res.ok, res.message.c_str());
    if (!res.ok) { printf("[FAIL] 启动失败\n"); return 1; }

    // 等待 agent bridge (18765) 与 gateway.lock 出现
    bool bridge = false;
    for (int i = 0; i < 20; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        auto occ = net::pidByPort(18765);
        if (occ) { bridge = true; printf("  t=%ds: agent-bridge(18765) UP pid=%lu\n", (i+1)*3, *occ); break; }
    }
    printf("agent-bridge(18765): %s\n", bridge ? "UP" : "DOWN");

    std::this_thread::sleep_for(std::chrono::seconds(5));
    HermesState st = launcher.state();
    printf("state() = %s\n", st==HermesState::Running ? "Running" : (st==HermesState::Stopped?"Stopped":"Other"));
    printf("gateway.lock 存在: %d\n", (int)std::filesystem::exists(Paths::dataDir()/"gateway.lock"));

    // 停止
    bool stopped = launcher.stop();
    printf("stop() = %d\n", (int)stopped);
    std::this_thread::sleep_for(std::chrono::seconds(2));
    printf("停止后 state = %s\n", launcher.state()==HermesState::Running ? "Running" : "Stopped");

    printf(bridge ? "\n[PASS] agent bridge 已拉起\n" : "\n[FAIL] agent bridge 未拉起\n");
    return bridge ? 0 : 1;
}
