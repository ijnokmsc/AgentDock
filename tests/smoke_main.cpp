// 冒烟测试: 直接调用核心层, 验证路径解析 + 组件探测 + 插件加载。
// 编译: 与 HermesStudio 同工程, 用宏 HS_SMOKE 单独编译 (不建 Qt 主窗口)。
#include <cstdio>
#include <string>
#include <vector>

#include "core/Updater.h"
#include "core/PluginHost.h"
#include "app/Paths.h"

using namespace hs;

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    fs::path root = "I:/HermesPortable";
    if (!Paths::setRoot(root)) {
        printf("[FAIL] setRoot 失败\n");
        return 1;
    }
    printf("[OK] root = %s\n", Paths::root().string().c_str());
    printf("     layout = %s\n", Paths::isOfficialLayout() ? "OFFICIAL" : (Paths::isLegacyLayout() ? "LEGACY" : "UNKNOWN"));
    printf("     venvPython    = %s\n", Paths::venvPython().string().c_str());
    printf("     hermesExe     = %s\n", Paths::hermesExe().string().c_str());
    printf("     portablePython= %s\n", Paths::portablePython().string().c_str());
    printf("     webUiEntry    = %s\n", Paths::webUiEntry().string().c_str());

    // 1) 探测修复
    printf("\n--- 组件探测 ---\n");
    Updater u;
    auto states = u.scanLocal();
    bool allOk = true;
    for (const auto& s : states) {
        printf("  %-16s 版本=%-10s 支持=%s\n",
               s.name.c_str(),
               s.currentVersion.empty() ? "(空)" : s.currentVersion.c_str(),
               s.supported ? "yes" : "NO");
        if (s.id == ComponentId::NodeRuntime || s.id == ComponentId::PythonRuntime) {
            if (!s.supported) allOk = false;
        }
    }
    printf(allOk ? "\n[PASS] 运行时探测全部通过\n"
                 : "\n[FAIL] 仍有运行时未探测到\n");

    // 2) 沙箱环境表 (Launcher 与 PortableBuilder / Hermes.bat 共用同一张表)
    printf("\n--- 沙箱环境变量 ---\n");
    for (const auto& e : Paths::sandboxEnvVars()) {
        printf("  %-24s = %s\n", e.first.c_str(), e.second.c_str());
    }

    // 3) 插件扫描
    printf("\n--- 插件 ---\n");
    PluginHost ph;
    auto plugins = ph.discover();
    printf("  发现插件 %zu 个\n", plugins.size());

    printf("\n[DONE]\n");
    return allOk ? 0 : 2;
}
