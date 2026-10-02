#pragma once
// 便携包「契约」的唯一实现点。
//
// 有三件事必须**构建期**(PortableBuilder) 和**运行期**(Launcher) 用同一套逻辑做,
// 否则就会重演「手动打包能跑、启动器一跑就漏」的漂移:
//
//   1. 沙箱目录必须先于任何进程存在。
//      某些 runtime 在 %TEMP% 不存在时直接启动失败, 所以是「先建再指」而非反之。
//
//   2. `_home\.hermes` 是指向 `data` 的 junction —— 它的目标是**绝对路径**,
//      便携包一旦换盘符/换目录就失效。它不是一次性的构建产物, 而是每次启动
//      都要对账的运行期不变量 (和 pyvenv.cfg 的 home 同一类问题)。
//
//   3. Hermes.bat 里钉的环境变量必须与 Launcher 注入的一致。
//      两边都从 `Paths::sandboxVarTable()` 渲染, 只有一份清单。

#include <string>
#include <filesystem>

namespace fs = std::filesystem;

namespace hs::portable {

// 建出 <root>/_home 下全部沙箱目录, 并保证 .hermes junction 指向 <root>/data。
// 幂等, 每次启动都可以调。失败时 err 给出原因。
bool ensureSandbox(const fs::path& root, std::string* err = nullptr);

// 只对账 _home\.hermes -> data 这一个 junction。
//   - 不存在            -> 创建
//   - 已指向正确 target -> 直接返回 (不动)
//   - 指向别处 (旧盘符) -> 删掉重链
//   - 是真实目录        -> 空的就换成 junction; 非空则原样保留 (绝不碰用户数据)
bool ensureDataJunction(const fs::path& root, std::string* err = nullptr);

// 生成纯 ASCII 的 Hermes.bat (CRLF 换行)。
//
// **必须纯 ASCII**: cmd.exe 按系统 ACP (中文 Windows 上是 GBK) 解析 .bat,
// 任何非 ASCII 字符 (哪怕在 rem 注释里) 都可能把行切碎成乱码命令。
// 这个坑已经在项目里踩过两次, 不要改回去。
std::string hermesBatScript();

} // namespace hs::portable
