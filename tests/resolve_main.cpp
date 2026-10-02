// 只读验证 PortableBuilder::resolveLatest() 的版本发现逻辑。
//
// 这条链路是构建器里最容易出错、又最难靠编译发现的一段:
//   GitHub releases 列表是**时间倒序**, 并且同时混着 runtime release
//   (tag = hermes-<ver>-runtime) / web-ui release (tag = v<ver>) / Android
//   release —— 不能用 /releases/latest (它可能返回只有 web-ui 资产的 tag)。
//   正确做法: 往下扫, 取第一个带 hermes-runtime-<platform>.json 的,
//             以及第一个带 hermes-web-ui-*.json 的。
//
// 不做任何下载, 不写盘, 只打印解析结果。
#include <cstdio>
#include <string>

#include "core/PortableBuilder.h"

using namespace hs;

int main(int argc, char** argv) {
    const std::string platform = (argc > 1) ? argv[1] : "win-x64";
    const std::string mirror   = (argc > 2) ? argv[2] : "";

    PortableBuilder pb;
    auto info = pb.resolveLatest(platform, mirror);

    printf("resolveLatest(platform=%s, mirror=%s)\n", platform.c_str(),
           mirror.empty() ? "(none)" : mirror.c_str());
    printf("  ok      = %s\n", info.ok ? "true" : "false");
    printf("  message = %s\n", info.message.c_str());

    if (!info.ok) {
        printf("[FAIL] 版本发现失败\n");
        return 1;
    }

    printf("\n  runtime:\n");
    printf("    tag     = %s\n", info.runtimeTag.c_str());
    printf("    version = %s\n", info.runtimeVersion.c_str());
    printf("    size    = %lld B (%.1f MB)\n", info.runtimeSize,
           info.runtimeSize / 1024.0 / 1024.0);
    printf("    sha256  = %s\n", info.runtimeSha256.c_str());
    printf("    url     = %s\n", info.runtimeUrl.c_str());
    printf("\n  web-ui:\n");
    printf("    tag     = %s\n", info.webUiTag.c_str());
    printf("    version = %s\n", info.webUiVersion.c_str());
    printf("    size    = %lld B (%.1f MB)\n", info.webUiSize,
           info.webUiSize / 1024.0 / 1024.0);
    printf("    sha256  = %s\n", info.webUiSha256.c_str());
    printf("    url     = %s\n", info.webUiUrl.c_str());
    printf("\n  manifestJson = %zu B\n", info.manifestJson.size());

    // 硬校验: 四要素齐全才算真的可用
    const bool complete = !info.runtimeUrl.empty() && !info.runtimeSha256.empty() &&
                          info.runtimeSize > 0 &&
                          !info.webUiUrl.empty() && !info.webUiSha256.empty() &&
                          info.webUiSize > 0;
    printf(complete ? "\n[PASS] runtime + web-ui 资产四要素齐全\n"
                    : "\n[FAIL] 资产字段不完整\n");
    return complete ? 0 : 2;
}
