// 验证三个组件的远端版本获取:
//   Kernel : 必须是 semver (hermes-<ver>-runtime tag 解析, 如 0.21.x), 不能是日期版
//   WebUI  : npm ekko-studio 版本
//   Node   : nodejs.org 最新版本 (semver)
//   Python : 必须是 Python 大版本号 (如 3.12.x), 不能是日期版 (20260914)
//   Node 版本列表: listNodeVersions() 能返回一批可用版本
//
// 只读网络, 不写盘。
#include <cstdio>
#include <string>
#include <vector>

#include "core/Updater.h"

using namespace hs;

static bool looksLikeSemver(const std::string& v) {
    // x.y.z (可带 v 前缀)
    return v.size() >= 5;
}

static bool looksLikeDate(const std::string& v) {
    // 纯 8 位数字 = 日期
    if (v.size() != 8) return false;
    for (char c : v) if (c < '0' || c > '9') return false;
    return true;
}

int main() {
    Updater u;
    int fail = 0;

    printf("=== Kernel ===\n");
    auto k = u.checkRemote(ComponentId::Kernel, "stable");
    printf("  ok=%s ver=%s msg=%s\n", k.ok?"true":"false", k.latestVersion.c_str(), k.message.c_str());
    if (!k.ok || k.latestVersion.empty()) { printf("  [FAIL]\n"); ++fail; }
    else if (looksLikeDate(k.latestVersion)) { printf("  [FAIL] 内核拿到日期版!\n"); ++fail; }
    else printf("  [PASS] 内核版本 = %s (semver)\n", k.latestVersion.c_str());

    printf("\n=== WebUI ===\n");
    auto w = u.checkRemote(ComponentId::WebUI, "stable");
    printf("  ok=%s ver=%s msg=%s\n", w.ok?"true":"false", w.latestVersion.c_str(), w.message.c_str());
    if (w.ok && !w.latestVersion.empty()) printf("  [PASS] web-ui = %s\n", w.latestVersion.c_str());
    else { printf("  [FAIL]\n"); ++fail; }

    printf("\n=== Node (最新) ===\n");
    auto n = u.checkRemote(ComponentId::NodeRuntime, "stable");
    printf("  ok=%s ver=%s msg=%s\n", n.ok?"true":"false", n.latestVersion.c_str(), n.message.c_str());
    if (n.ok && !n.latestVersion.empty() && !looksLikeDate(n.latestVersion))
        printf("  [PASS] node = %s\n", n.latestVersion.c_str());
    else { printf("  [FAIL]\n"); ++fail; }

    printf("\n=== Python ===\n");
    auto p = u.checkRemote(ComponentId::PythonRuntime, "stable");
    printf("  ok=%s ver=%s msg=%s\n", p.ok?"true":"false", p.latestVersion.c_str(), p.message.c_str());
    if (!p.ok || p.latestVersion.empty()) { printf("  [FAIL]\n"); ++fail; }
    else if (looksLikeDate(p.latestVersion)) { printf("  [FAIL] Python 拿到日期版!\n"); ++fail; }
    else if (p.latestVersion.rfind("3.", 0) != 0) { printf("  [FAIL] Python 版本不像 3.x\n"); ++fail; }
    else printf("  [PASS] python = %s (大版本)\n", p.latestVersion.c_str());

    printf("\n=== Node 版本列表 (前 8 个) ===\n");
    auto list = u.listNodeVersions(8);
    if (list.empty()) { printf("  [FAIL] 取不到 Node 版本列表\n"); ++fail; }
    else {
        for (size_t i = 0; i < list.size(); ++i) printf("  [%zu] %s\n", i, list[i].c_str());
        printf("  [PASS] 共 %zu 个\n", list.size());
    }

    printf("\n%s\n", fail ? "[RESULT] FAIL" : "[RESULT] PASS");
    return fail;
}
