#include "Updater.h"
#include "UpdateManager.h"
#include "PortableBuilder.h"
#include "../platform/ProcessUtil.h"
#include "../platform/HttpClient.h"
#include "../platform/Archive.h"
#include "../platform/Hash.h"
#include "../storage/JsonStore.h"

#include <windows.h>   // GetCurrentProcessId / GetSystemDirectoryW / MAX_PATH

#include <fstream>
#include <sstream>
#include <chrono>
#include <regex>
#include <algorithm>
#include <charconv>

namespace {

// 当前构建平台的 asset 平台串 (PortableBuilder 的 runtime 清单名后缀)。
// 启动器只发 win-x64, 这里写死; 若将来做 arm64 再扩展。
std::string platform() { return "win-x64"; }

// 启动器发布仓库 (owner/repo): 自更新检测与下载源。
// 仓库改名/转移时必须同步这里 (checkRemote / applyLauncherUpdate 共用)。
constexpr const char* kLauncherRepo = "ijnokmsc/AgentDock";

// Launcher 组件的当前版本。launcher_v3 构建注入 HERMES_V3_VERSION (exe 真实
// 版本); Studio 宿主构建无此宏才退回 HERMESSTUDIO_VERSION —— 不能直接用后者:
// launcher_v3 未定义它, HttpClient.h 会兜底成 "0.0.0", 自更新比对必然误报。
std::string launcherVersion() {
#ifdef HERMES_V3_VERSION
    return HERMES_V3_VERSION;
#else
    return HERMESSTUDIO_VERSION;
#endif
}

// 执行 "<exe> <arg>" 并从管道捕获输出, 提取其中的版本号。
//
// 注意: 早期实现走 "cmd /c exe --version > tmpfile", 但 buildCommandLine 会给每个
// 参数加引号做转义, 导致 cmd 收到的是被二次转义的整串 -> 重定向失效、输出永远为空,
// node / python / uv 三个运行时因此在更新页全部显示"未探测到"。改用管道直读。
std::optional<std::string> runVersionRedirect(const fs::path& exe, const std::string& arg) {
    std::error_code ec;
    if (exe.empty() || !fs::exists(exe, ec)) return std::nullopt;

    hs::proc::LaunchOptions opt;
    opt.exe    = exe;
    opt.args   = {arg};
    opt.hidden = true;

    auto r = hs::proc::runAndCapture(opt, 15000);
    static const std::regex re(R"(v?(\d+(?:\.\d+){0,3}))");
    std::smatch m;
    if (std::regex_search(r.output, m, re)) return m[1].str();
    return std::nullopt;
}

std::vector<int> parseParts(const std::string& v) {
    std::vector<int> out;
    std::string cur;
    for (char c : v) {
        if (c == '.') { if (!cur.empty()) { out.push_back(std::atoi(cur.c_str())); cur.clear(); } }
        else if (std::isdigit((unsigned char)c)) cur += c;
        else break;   // 遇到预发布后缀就停止 (alpha/beta 不在 M0 处理范围)
    }
    if (!cur.empty()) out.push_back(std::atoi(cur.c_str()));
    return out;
}
} // namespace

namespace hs {

int compareVersion(const std::string& a, const std::string& b) {
    // 纯日期版本 (python-build-standalone: 20250909)
    auto isDate = [](const std::string& s) {
        return s.size() == 8 && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
    };
    if (isDate(a) && isDate(b)) return a.compare(b) < 0 ? -1 : (a == b ? 0 : 1);

    auto pa = parseParts(a), pb = parseParts(b);
    size_t n = std::max(pa.size(), pb.size());
    for (size_t i = 0; i < n; ++i) {
        int x = i < pa.size() ? pa[i] : 0;
        int y = i < pb.size() ? pb[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

const std::vector<Updater::ComponentDef>& Updater::definitions() {
    static const std::vector<ComponentDef> defs = {
        {ComponentId::Launcher,      "AgentDock 启动器",    "编译期常量",                false},
        {ComponentId::Kernel,        "Hermes 内核",         "hermes.exe --version",      false},
        {ComponentId::WebUI,         "Web UI",              "hermes-web-ui/package.json", false},
        {ComponentId::NodeRuntime,   "Node.js 运行时",       "node.exe --version",        false},
        // 注: 不再列 uv —— 官方 runtime 发行包 (python/ node/ git/) 里没有 uv,
        // 它只在构建期用来建 venv (pyvenv.cfg 里的 uv = x.y.z 仅是创建者标记),
        // 运行期用不到。列出来只会让体检页误报「未探测到」。
        {ComponentId::PythonRuntime, "Python 运行时",        "python.exe --version",      true},
    };
    return defs;
}

std::optional<Updater::ComponentDef> Updater::defOf(ComponentId id) {
    for (const auto& d : definitions()) if (d.id == id) return d;
    return std::nullopt;
}

Updater::Updater() {}

std::optional<std::string> Updater::probeKernel() const {
    // 1) hermes.exe --version 最准
    if (auto v = runVersionRedirect(Paths::hermesExe(), "--version")) return v;
    // 2) 回退: data/.update_check 里缓存的版本
    auto j = JsonStore::load(Paths::dataDir() / ".update_check");
    if (j && j->contains("ver") && (*j)["ver"].is_string()) {
        return (*j)["ver"].get<std::string>();
    }
    return std::nullopt;
}

std::optional<std::string> Updater::probeWebUi() const {
    auto j = JsonStore::load(Paths::webUiDir() / "package.json");
    if (j && j->contains("version") && (*j)["version"].is_string()) {
        return (*j)["version"].get<std::string>();
    }
    return std::nullopt;
}

std::optional<std::string> Updater::probePython() const {
    fs::path py = Paths::portablePython();
    if (py.empty()) py = Paths::venvPython();
    return runVersionRedirect(py, "--version");
}

std::optional<std::string> Updater::probeNode() const {
    return runVersionRedirect(Paths::nodeExe(), "--version");
}

std::vector<ComponentState> Updater::scanLocal() {
    std::vector<ComponentState> out;

    auto add = [&](ComponentId id, std::optional<std::string> ver, fs::path path, std::string note = {}) {
        auto d = defOf(id);
        ComponentState s;
        s.id = id;
        s.name = d ? d->name : toString(id);
        s.currentVersion = ver.value_or("");
        s.installPath = path.string();
        s.note = note;
        s.supported = ver.has_value();
        out.push_back(s);
    };

    add(ComponentId::Launcher, launcherVersion(), fs::path{}, "启动器自身");
    add(ComponentId::Kernel,  probeKernel(),  Paths::hermesAgentDir());
    add(ComponentId::WebUI,   probeWebUi(),   Paths::webUiDir());
    add(ComponentId::NodeRuntime, probeNode(), Paths::nodeExe());
    add(ComponentId::PythonRuntime, probePython(), Paths::pythonDir());

    return out;
}

std::vector<ComponentState> Updater::scanWithRemote(const std::string& channel) {
    auto states = scanLocal();
    for (auto& s : states) {
        auto r = checkRemote(s.id, channel);
        if (r.ok) {
            s.latestVersion = r.latestVersion;
            // 版本比对: 当前非空且最新非空时判断是否有更新
            if (!s.currentVersion.empty() && !s.latestVersion.empty()) {
                // 内核最新现在也是 semver (来自 hermes-<ver>-runtime tag),
                // 与本地 0.21.3 可直接比较 —— 不再需要日期版特判。
                s.updateAvailable = compareVersion(s.latestVersion, s.currentVersion) > 0;
            }
        }
    }
    return states;
}

Updater::RemoteResult Updater::checkRemote(ComponentId id, const std::string& channel) {
    RemoteResult r;
    r.ok = false;

    // 用 WinHTTP 网络层查各组件最新版本
    try {
        switch (id) {
            case ComponentId::Kernel: {
                // Hermes 内核发布版的权威来源是 EKKOLearnAI/hermes-studio 的
                // `hermes-<semver>-runtime` release (与 PortableBuilder 同一份)。
                // 不能用 NousResearch/hermes-agent —— 那个的 tag 是日期版 (2026.9.14),
                // 和本地 semver (0.21.3) 无法比较, 也不对应官方 runtime 发行包。
                // 我们只需要版本号, 资产清单由 PortableBuilder 负责, 这里直接调它。
                PortableBuilder pb;
                auto info = pb.resolveLatest(platform());
                if (!info.ok) { r.message = info.message; return r; }
                r.latestVersion = info.runtimeVersion;
                r.ok = true;
                r.message = "Hermes 内核最新: " + info.runtimeVersion;
                break;
            }
            case ComponentId::WebUI: {
                // npm registry ekko-studio
                auto resp = net::httpGet("https://registry.npmjs.org/ekko-studio/latest");
                if (!resp.ok) { r.message = "npm 查询失败: " + resp.error; return r; }
                std::string ver;
                auto p = resp.body.find("\"version\"");
                if (p != std::string::npos) {
                    auto q1 = resp.body.find('"', p + 9);
                    auto q2 = q1 == std::string::npos ? std::string::npos : resp.body.find('"', q1 + 1);
                    if (q1 != std::string::npos && q2 != std::string::npos)
                        ver = resp.body.substr(q1 + 1, q2 - q1 - 1);
                }
                if (ver.empty()) { r.message = "无法解析 npm version"; return r; }
                r.latestVersion = ver;
                r.ok = true;
                r.message = "npm 最新: " + ver;
                break;
            }
            case ComponentId::NodeRuntime: {
                // nodejs.org 官方 dist
                auto resp = net::httpGet("https://nodejs.org/dist/index.json");
                if (!resp.ok) { r.message = "nodejs.org 查询失败"; return r; }
                // index.json 数组, 第一个是 latest。解析 "version":"v22.x.x"
                auto p = resp.body.find("\"version\"");
                if (p != std::string::npos) {
                    auto q1 = resp.body.find('"', p + 9);
                    auto q2 = q1 == std::string::npos ? std::string::npos : resp.body.find('"', q1 + 1);
                    if (q1 != std::string::npos && q2 != std::string::npos) {
                        std::string ver = resp.body.substr(q1 + 1, q2 - q1 - 1);
                        if (!ver.empty() && ver[0] == 'v') ver = ver.substr(1);
                        r.latestVersion = ver;
                        r.ok = true;
                        r.message = "node 最新: " + ver;
                    } else { r.message = "node 版本解析失败"; }
                } else { r.message = "node 版本解析失败"; }
                break;
            }
            case ComponentId::PythonRuntime: {
                // python-build-standalone: 它的 release tag 是**日期版** (如 20260901),
                // 不是 Python 版本号 —— 直接展示会误导用户。真正的 Python 版本藏在
                // 资产名里, 形如 `cpython-3.12.14+20260901-x86_64-pc-windows-msvc-install_only.tar.gz`。
                // 且一个日期 release 会同时带 3.10~3.15 多个 Python 小版本。
                // 展示: 取该 release 里**最高**的 Python 版本号 (避免只取到 3.10.x 让用户误判降级);
                // 但更新下载仍按日期 tag 走 (python-build-standalone 按日期发行)。
                auto resp = net::httpGet("https://api.github.com/repos/astral-sh/python-build-standalone/releases/latest");
                if (!resp.ok) { r.message = "python 查询失败"; return r; }
                static const std::regex reTag("\"tag_name\"\\s*:\\s*\"v?([0-9]{8})\"");
                static const std::regex reCpython("cpython-(3\\.[0-9]+\\.[0-9]+)\\+");
                std::smatch mTag;
                if (!std::regex_search(resp.body, mTag, reTag)) { r.message = "python 版本解析失败"; return r; }
                const std::string dateTag = mTag[1].str();
                // 收集 body 里所有 cpython-3.x.y 版本, 取版本最高者 (先转整型三元组比较)
                int best[3] = {0,0,0};
                auto take = [&](const std::string& v) {
                    int a=0,b=0,c=0;
                    std::istringstream ss(v);
                    char d1, d2;
                    ss >> a >> d1 >> b >> d2 >> c;
                    if (d1=='.' && d2=='.') {
                        if (a>best[0] || (a==best[0]&&b>best[1]) || (a==best[0]&&b==best[1]&&c>best[2])) { best[0]=a;best[1]=b;best[2]=c; }
                    }
                };
                for (std::sregex_iterator it(resp.body.begin(), resp.body.end(), reCpython), end; it != end; ++it) {
                    take((*it)[1].str());
                }
                std::string pyVer;
                if (best[0] > 0) pyVer = std::to_string(best[0]) + "." + std::to_string(best[1]) + "." + std::to_string(best[2]);
                // 若 latest 的 body 里没展开资产名 (assets 默认只给 URL), 退一次查 release 详情页
                if (pyVer.empty()) {
                    auto p2 = resp.body.find("\"assets_url\"");
                    if (p2 != std::string::npos) {
                        auto q1 = resp.body.find('"', p2 + 13);
                        auto q2 = q1 == std::string::npos ? std::string::npos : resp.body.find('"', q1 + 1);
                        if (q1 != std::string::npos && q2 != std::string::npos) {
                            auto ra = net::httpGet(resp.body.substr(q1 + 1, q2 - q1 - 1));
                            if (ra.ok) {
                                for (std::sregex_iterator it(ra.body.begin(), ra.body.end(), reCpython), end; it != end; ++it) {
                                    take((*it)[1].str());
                                }
                                if (best[0] > 0) pyVer = std::to_string(best[0]) + "." + std::to_string(best[1]) + "." + std::to_string(best[2]);
                            }
                        }
                    }
                }
                // 展示: 优先最高 Python 大版本; 拿不到就退回日期 tag, 但标清楚是日期。
                r.latestVersion = pyVer.empty() ? dateTag : pyVer;
                r.ok = true;
                r.message = pyVer.empty()
                    ? ("python 最新: " + dateTag + " (日期, 未解析出版本号)")
                    : ("python 最新: " + pyVer + "  (" + dateTag + ")");
                break;
            }
            case ComponentId::Launcher: {
                // 启动器自更新: 查自身发布仓库 releases/latest, tag 形如 v0.1.0。
                // 仓库公开时无需鉴权; 私有/不存在时 HTTP 非 200, 走查询失败分支。
                auto resp = net::httpGet(std::string("https://api.github.com/repos/")
                                         + kLauncherRepo + "/releases/latest");
                if (!resp.ok) { r.message = "GitHub 查询失败: " + resp.error; return r; }
                auto p = resp.body.find("\"tag_name\"");
                if (p == std::string::npos) { r.message = "发布仓库暂无 release"; return r; }
                auto q1 = resp.body.find('"', p + 10);
                auto q2 = q1 == std::string::npos ? std::string::npos : resp.body.find('"', q1 + 1);
                if (q1 == std::string::npos || q2 == std::string::npos) {
                    r.message = "无法解析 tag_name";
                    return r;
                }
                std::string tag = resp.body.substr(q1 + 1, q2 - q1 - 1);
                if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) tag.erase(0, 1);
                r.latestVersion = tag;
                r.ok = true;
                r.message = "启动器最新: " + tag;
                break;
            }
            default:
                r.message = "暂不支持的组件: " + toString(id);
                break;
        }
    } catch (const std::exception& e) {
        r.message = "网络异常: " + std::string(e.what());
    }
    return r;
}

Updater::ApplyResult Updater::applyKernelUpdate(
        const std::string& targetVersion,
        std::function<void(int, const std::string&)> progress) {
    ApplyResult r;
    UpdateManager um;
    std::error_code ec;

    const fs::path target  = Paths::hermesAgentDir();
    std::error_code ec2;
    const bool official = fs::is_directory(target / "hermes_cli", ec2);
    // 备份 tag 语义 = 被备份的(当前)版本, 不是目标版本
    const std::string versionTag = "pre-" + probeKernel().value_or("unknown");

    // 1) 下载源码/运行包
    fs::path dlDir = Paths::downloadDir();
    fs::create_directories(dlDir, ec);
    fs::path dlFile;
    std::string url;
    if (progress) progress(5, "解析发行包");
    if (official) {
        // 官方布局: 内核更新 = 拿该版本 runtime 发行包里的 hermes_cli 源码部分,
        // 而非 NousResearch 的 zipball (它没有 semver tag, 且不含官方布局)。
        PortableBuilder pb;
        auto info = pb.resolveLatest(platform());
        if (!info.ok) { r.message = "解析版本失败: " + info.message; return r; }
        if (info.runtimeUrl.empty()) { r.message = "未找到 runtime 发行包地址"; return r; }
        url = info.runtimeUrl;
        dlFile = dlDir / ("hermes-runtime-" + targetVersion + ".tar.gz");
    } else {
        // 旧布局: 仍从源码仓库拉 zipball
        url = "https://api.github.com/repos/NousResearch/hermes-agent/zipball/v" + targetVersion;
        dlFile = dlDir / ("hermes-agent-" + targetVersion + ".zip");
    }
    if (progress) progress(10, "下载发行包");
    auto dl = net::downloadFile(url, dlFile, {}, [&](int p, std::int64_t, std::int64_t){
        if (progress) progress(10 + p * 15 / 100, "下载中");
        return true;
    });
    if (!dl.ok) { r.message = "下载失败: " + dl.error; return r; }

    // 2) 解压到临时目录
    fs::path tmp = dlDir / ("kernel_tmp_" + targetVersion);
    fs::remove_all(tmp, ec);
    if (progress) progress(30, "解压发行包");
    auto arcErr = hs::arc::extractAuto(dlFile, tmp);
    if (arcErr) { r.message = "解压失败: " + *arcErr; return r; }

    // 3) 定位 hermes-agent 源码树根
    //    官方 runtime 包: 解压出 ./python ./node ./git, 源码树在 python/ 下
    //    旧 zipball:       解压出顶层单目录, 即源码树根
    fs::path srcTree;
    if (official) {
        srcTree = tmp / "python";
        if (!fs::is_directory(srcTree, ec)) { r.message = "runtime 包缺少 python/ 目录"; return r; }
    } else {
        for (fs::directory_iterator it(tmp, ec), end; it != end; ++it) {
            if (it->is_directory(ec)) { srcTree = it->path(); break; }
        }
        if (srcTree.empty()) { r.message = "源码包结构异常"; return r; }
    }

    // 4) 备份 + 替换
    //
    // 官方布局下 <root>/python 既是 hermes-agent 源码树根, 又装着解释器
    // base/ 与 venv/。整目录删掉会把运行时一起毁掉 —— 所以走「逐项覆盖」,
    // 并显式跳过 base/ 与 venv/。旧布局 (<root>/hermes-agent) 仍整目录替换。
    if (official) {
        static const char* kProtected[] = {"base", "venv"};
        auto isProtected = [](const std::string& n) {
            for (auto* p : kProtected) if (n == p) return true;
            return false;
        };

        // 4a) 备份当前源码 (跳过运行时目录, 否则备份会白白多出 1.5GB)
        if (progress) progress(55, "备份旧内核源码");
        fs::path backup = um.backupDir("kernel", versionTag);
        fs::remove_all(backup, ec);
        fs::create_directories(backup, ec);
        int backed = 0;
        for (fs::directory_iterator it(target, ec), end; it != end; ++it) {
            const std::string name = it->path().filename().string();
            if (isProtected(name)) continue;
            fs::path to = backup / it->path().filename();
            if (it->is_directory(ec)) {
                fs::copy(it->path(), to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
            } else {
                fs::copy_file(it->path(), to, fs::copy_options::overwrite_existing, ec);
            }
            if (ec) { r.message = "备份失败(" + name + "): " + ec.message(); return r; }
            ++backed;
        }
        if (backed == 0) { r.message = "备份失败: 目标目录里没有可备份的源码"; return r; }
        r.backup = backup;

        // 手工回滚 (备份目录没写 UpdateManager 的 manifest, 不能用 um.rollback)
        auto restore = [&]() -> bool {
            std::error_code e2;
            bool ok = true;
            for (fs::directory_iterator it(backup, e2), end; it != end; ++it) {
                fs::path dst = target / it->path().filename();
                fs::remove_all(dst, e2); e2.clear();
                if (it->is_directory(e2)) {
                    fs::copy(it->path(), dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, e2);
                } else {
                    fs::copy_file(it->path(), dst, fs::copy_options::overwrite_existing, e2);
                }
                if (e2) ok = false;
            }
            return ok;
        };

        // 4b) 逐项覆盖 (先删同名再拷, 保证删除的文件真的被删掉)
        if (progress) progress(70, "覆盖内核源码");
        int applied = 0;
        for (fs::directory_iterator it(srcTree, ec), end; it != end; ++it) {
            const std::string name = it->path().filename().string();
            if (isProtected(name)) continue;   // 防御: 源码包里出现同名目录也不许覆盖运行时
            fs::path dst = target / it->path().filename();
            fs::remove_all(dst, ec);
            ec.clear();
            fs::rename(it->path(), dst, ec);
            if (ec) {
                ec.clear();
                if (it->is_directory(ec)) {
                    fs::create_directories(dst, ec);
                    fs::copy(it->path(), dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
                } else {
                    fs::copy_file(it->path(), dst, fs::copy_options::overwrite_existing, ec);
                }
            }
            if (ec) {
                r.message = "覆盖 " + name + " 失败: " + ec.message();
                if (restore()) r.message += " — 已回滚到原版本";
                return r;
            }
            ++applied;
        }
        if (applied == 0) {
            r.message = "源码包结构异常 (无可覆盖项)";
            if (restore()) r.message += " — 已回滚";
            return r;
        }
    } else {
        // 旧布局: 整目录替换
        if (progress) progress(55, "备份旧版本");
        auto lastTickK = std::chrono::steady_clock::now();
        auto backupProgK = [&](std::uintmax_t bytes) {
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTickK).count() < 200) return;
            lastTickK = now;
            progress(56, "备份旧版本: 已复制 " + std::to_string(bytes / (1024 * 1024)) + " MB ...");
        };
        auto backup = um.backupComponent(target, "kernel", versionTag, nullptr, backupProgK);
        if (backup.empty()) { r.message = "备份旧版本失败"; return r; }
        r.backup = backup;

        if (progress) progress(70, "替换内核");
        fs::remove_all(target, ec);
        fs::rename(srcTree, target, ec);
        if (ec) {
            // rename 失败 (跨盘), 退回复制
            ec.clear();
            fs::create_directories(target, ec);
            fs::copy(srcTree, target, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        }
        if (ec) {
            r.message = "替换失败: " + ec.message();
            if (um.rollback(backup)) r.message = "替换失败, 已回滚到原版本";
            return r;
        }
    }

    // 5) 清理临时
    fs::remove_all(tmp, ec);
    fs::remove(dlFile, ec);
    if (progress) progress(100, "完成");
    r.ok = true;
    r.message = "内核已更新到 " + targetVersion + " (重启 Hermes 生效)";
    return r;
}

Updater::ApplyResult Updater::applyWebUiUpdate(
        const std::string& targetVersion,
        std::function<void(int, const std::string&)> progress) {
    ApplyResult r;
    UpdateManager um;
    std::error_code ec;

    // web-ui 是 npm 包 ekko-studio。npm tarball 下载: https://registry.npmjs.org/<pkg>/-/<pkg>-<ver>.tgz
    fs::path dlDir = Paths::downloadDir();
    fs::create_directories(dlDir, ec);
    fs::path tgz = dlDir / ("ekko-studio-" + targetVersion + ".tgz");
    if (progress) progress(5, "下载 web-ui 包");
    std::string url = "https://registry.npmjs.org/ekko-studio/-/ekko-studio-" + targetVersion + ".tgz";
    auto dl = net::downloadFile(url, tgz, {}, [&](int p, std::int64_t, std::int64_t){
        if (progress) progress(5 + p * 20 / 100, "下载中");
        return true;
    });
    if (!dl.ok) { r.message = "下载失败: " + dl.error; return r; }

    // 解压 npm tgz (tar.gz, 顶层是 package/)
    fs::path tmp = dlDir / ("webui_tmp_" + targetVersion);
    fs::remove_all(tmp, ec);
    if (progress) progress(30, "解压 web-ui");
    auto arcErr = hs::arc::extractAuto(tgz, tmp);
    if (arcErr) { r.message = "解压失败: " + *arcErr; return r; }
    fs::path pkg = tmp / "package";
    if (!fs::is_directory(pkg)) { r.message = "npm 包结构异常 (无 package/ 目录)"; return r; }

    // 备份当前 web-ui (web-ui 目录数百 MB, 备份 1-2 分钟; 字节级进度透出)
    if (progress) progress(55, "备份旧版本");
    auto lastTick = std::chrono::steady_clock::now();
    auto backupProg = [&](std::uintmax_t bytes) {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTick).count() < 200) return;
        lastTick = now;
        progress(56, "备份旧版本: 已复制 " + std::to_string(bytes / (1024 * 1024)) + " MB ...");
    };
    fs::path target = Paths::webUiDir();
    auto backup = um.backupComponent(target, "webui",
                                     "pre-" + probeWebUi().value_or("unknown"), nullptr, backupProg);
    if (backup.empty()) { r.message = "备份旧版本失败"; return r; }
    r.backup = backup;

    // 替换: 旧树整体挪到临时位 (同卷 rename 即时), 换上新包后把 node_modules 移回。
    // npm 包不含依赖, 旧实现整目录删除会让 npm install 从零全量拉取 (数百 MB,
    // 慢且强依赖网络, 表现为"卡在安装依赖数分钟")。移回后 npm install 只做增量对齐。
    if (progress) progress(70, "替换 web-ui");
    fs::path oldTree = dlDir / ("webui_old_" + targetVersion);
    fs::remove_all(oldTree, ec);
    bool movedOld = false;
    fs::rename(target, oldTree, ec);
    if (!ec) movedOld = true;
    else { ec.clear(); fs::remove_all(target, ec); }   // 跨卷/被占用: 退回删除路径
    fs::rename(pkg, target, ec);
    if (ec) {
        ec.clear();
        fs::create_directories(target, ec);
        fs::copy(pkg, target, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    }
    if (ec) {
        r.message = "替换失败: " + ec.message();
        std::error_code ec2;
        if (movedOld) { fs::remove_all(target, ec2); fs::rename(oldTree, target, ec2); }
        if (um.rollback(backup)) r.message = "替换失败, 已回滚到原版本";
        return r;
    }
    if (movedOld) {
        fs::path nm = oldTree / "node_modules";
        if (fs::exists(nm, ec)) {
            fs::rename(nm, target / "node_modules", ec);   // 同卷 rename 即时
            if (ec) {
                ec.clear();
                fs::copy(nm, target / "node_modules",
                         fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
            }
        }
        fs::remove_all(oldTree, ec);
    }

    // npm 包不含 node_modules 的新增依赖仍需对齐 —— 旧依赖已就位, 这里只增量补差
    {
        fs::path nodeExe = Paths::nodeExe();
        fs::path npmCli = Paths::nodeDir() / "node_modules" / "npm" / "bin" / "npm-cli.js";
        if (fs::exists(nodeExe, ec) && fs::exists(npmCli, ec)) {
            if (progress) progress(80, "对齐 web-ui 依赖 (npm install, 增量)");
            proc::LaunchOptions opt;
            opt.exe        = nodeExe;
            opt.args       = { npmCli.string(), "install", "--omit=dev", "--no-audit",
                               "--no-fund", "--prefer-offline", "--loglevel=error" };
            opt.workingDir = target;
            opt.hidden     = true;
            opt.env        = Paths::sandboxEnvVars();
            // POSIX test 垫片前插 PATH: 包 prepare ("[ -d dist ] || npm run build")
            // 在 cmd 下无法解析 "[" -> 误触发需 devDeps 的完整构建 -> npm 必败
            if (fs::path shim = Paths::ensurePosixTestShimDir(); !shim.empty()) {
                const char* curPath = std::getenv("PATH");
                opt.env.push_back({"PATH", shim.string() + ";" + (curPath ? curPath : "")});
            }
            int code = 0;
            auto cap = proc::runAndCapture(opt, 15 * 60 * 1000, nullptr);
            code = cap.exitCode;
            if (code != 0 || !fs::exists(target / "node_modules", ec)) {
                // 失败详情 (exit code + npm 输出尾部) 必须保留 —— 此前被回滚成功
                // 的通用文案覆盖, 用户只看到"失败"看不到原因
                r.message = "依赖安装失败 (exit " + std::to_string(code) + ")";
                if (!cap.output.empty()) r.message += ": " + cap.output.substr(0, 300);
                r.message += um.rollback(backup)
                    ? ", 已回滚到原版本"
                    : ", 回滚失败 (备份: " + backup.string() + ")";
                return r;
            }
        }
    }
    fs::remove_all(tmp, ec);
    fs::remove(tgz, ec);
    if (progress) progress(100, "完成");
    r.ok = true;
    r.message = "Web UI 已更新到 " + targetVersion + " (重启生效)";
    return r;
}

Updater::ApplyResult Updater::applyNodeUpdate(
        const std::string& version,
        std::function<void(int, const std::string&)> progress) {
    ApplyResult r;
    UpdateManager um;
    std::error_code ec;

    // 1) 下载 nodejs.org 官方 Windows zip
    //    https://nodejs.org/dist/v<ver>/node-v<ver>-win-x64.zip
    fs::path dlDir = Paths::downloadDir();
    fs::create_directories(dlDir, ec);
    const std::string ver = (version[0] == 'v') ? version : ("v" + version);
    fs::path zip = dlDir / ("node-" + ver + "-win-x64.zip");
    std::string url = "https://nodejs.org/dist/" + ver + "/node-" + ver + "-win-x64.zip";
    if (progress) progress(5, "下载 Node " + ver);
    auto dl = net::downloadFile(url, zip, {}, [&](int p, std::int64_t, std::int64_t){
        if (progress) progress(5 + p * 25 / 100, "下载中");
        return true;
    });
    if (!dl.ok) { r.message = "下载失败: " + dl.error; return r; }

    // 2) 解压到临时目录
    fs::path tmp = dlDir / ("node_tmp_" + ver);
    fs::remove_all(tmp, ec);
    if (progress) progress(30, "解压 Node");
    auto arcErr = hs::arc::extractAuto(zip, tmp);
    if (arcErr) { r.message = "解压失败: " + *arcErr; return r; }

    // 3) 定位解压出的 node 根 (node-v<ver>-win-x64/)
    fs::path srcTree;
    for (fs::directory_iterator it(tmp, ec), end; it != end; ++it) {
        if (it->is_directory(ec)) { srcTree = it->path(); break; }
    }
    if (srcTree.empty()) { r.message = "node 包结构异常"; return r; }
    if (!fs::exists(srcTree / "node.exe", ec)) { r.message = "node 包缺少 node.exe"; return r; }

    // 4) 备份 + 整目录替换
    if (progress) progress(55, "备份旧 Node");
    fs::path target = Paths::nodeDir();
    auto lastTickN = std::chrono::steady_clock::now();
    auto backupProgN = [&](std::uintmax_t bytes) {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTickN).count() < 200) return;
        lastTickN = now;
        progress(56, "备份旧 Node: 已复制 " + std::to_string(bytes / (1024 * 1024)) + " MB ...");
    };
    // 备份 tag 语义 = 被备份的(当前)版本, 不是目标版本
    auto backup = um.backupComponent(target, "node",
                                     "pre-" + probeNode().value_or("unknown"), nullptr, backupProgN);
    if (backup.empty()) { r.message = "备份旧版本失败"; return r; }
    r.backup = backup;

    if (progress) progress(70, "替换 Node");
    fs::remove_all(target, ec);
    ec.clear();
    fs::rename(srcTree, target, ec);
    if (ec) {
        // 跨盘失败, 退回复制
        ec.clear();
        fs::create_directories(target, ec);
        fs::copy(srcTree, target, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    }
    if (ec) {
        r.message = "替换失败: " + ec.message();
        if (um.rollback(backup)) r.message = "替换失败, 已回滚到原版本";
        return r;
    }

    // 5) 清理
    fs::remove_all(tmp, ec);
    fs::remove(zip, ec);
    if (progress) progress(100, "完成");
    r.ok = true;
    r.message = "Node 已更新到 " + ver + " (重启生效)";
    return r;
}

std::vector<std::string> Updater::listNodeVersions(size_t maxCount) {
    std::vector<std::string> out;
    auto resp = net::httpGet("https://nodejs.org/dist/index.json");
    if (!resp.ok) return out;
    // index.json 是数组, 每项有 "version":"v22.22.0"。取前 maxCount 个。
    std::regex re("\"version\"\\s*:\\s*\"v([0-9]+\\.[0-9]+\\.[0-9]+)\"");
    for (std::sregex_iterator it(resp.body.begin(), resp.body.end(), re), end; it != end && out.size() < maxCount; ++it) {
        out.push_back((*it)[1].str());
    }
    return out;
}

// 启动器自更新: 下载 release 的 .exe 资产 -> 自替换。Windows 允许运行中的 exe
// 改名但不允许删除/覆盖, 所以走标准三步:
//   1) 当前 exe 改名为 <name>.old.exe 腾位 (被杀软短暂锁住时重试数次);
//   2) 下载的 <name>.new.exe 改名回原 exe 名; 失败则把 .old.exe 改回去 (回滚);
//   3) 重启后生效; .old.exe 保留作保底副本, 下次启动时清理 (main.cpp)。
// 不走 UpdateManager 备份: 它只支持目录树, 且单文件回滚语义对不上;
// .old.exe + release 资产本身就是足够的回滚路径。
Updater::ApplyResult Updater::applyLauncherUpdate(
        const std::string& targetVersion,
        std::function<void(int, const std::string&)> progress) {
    ApplyResult r;
    std::error_code ec;

    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    const fs::path exe = buf;
    if (exe.empty()) { r.message = "无法定位自身 exe"; return r; }
    fs::path newExe = exe; newExe.replace_extension(L".new.exe");
    fs::path oldExe = exe; oldExe.replace_extension(L".old.exe");

    // 1) 解析目标版本 release 资产 (取第一个 .exe)
    if (progress) progress(5, "解析发行资产");
    auto resp = net::httpGet(std::string("https://api.github.com/repos/") + kLauncherRepo
                             + "/releases/tags/v" + targetVersion);
    if (!resp.ok) { r.message = "GitHub 查询失败: " + resp.error; return r; }
    std::string url;
    std::regex reAsset("\"browser_download_url\"\\s*:\\s*\"([^\"]+\\.exe)\"");
    std::smatch m;
    if (std::regex_search(resp.body, m, reAsset) && m.size() > 1) url = m[1].str();
    if (url.empty()) { r.message = "release v" + targetVersion + " 未找到 .exe 资产"; return r; }

    // 2) 下载到 exe 同目录 (.new.exe); 失败清理半成品
    if (progress) progress(10, "下载新版启动器");
    auto dl = net::downloadFile(url, newExe, {}, [&](int p, std::int64_t, std::int64_t) -> bool {
        if (progress) progress(10 + p * 60 / 100, "下载中");
        return true;   // 不中断下载 (无取消入口)
    });
    if (!dl.ok) {
        std::error_code ec2; fs::remove(newExe, ec2);
        r.message = "下载失败: " + dl.error;
        return r;
    }

    // 3) 自替换
    if (progress) progress(80, "替换 exe");
    std::error_code ec3; fs::remove(oldExe, ec3);   // 清掉上次更新残留的保底副本
    bool renamed = false;
    for (int i = 0; i < 5 && !renamed; ++i) {
        ec.clear();
        fs::rename(exe, oldExe, ec);
        renamed = !ec;
        if (!renamed) Sleep(300 * (i + 1));
    }
    if (!renamed) {
        std::error_code ec2; fs::remove(newExe, ec2);
        r.message = "无法改名当前 exe (可能被杀软锁定), 已取消更新";
        return r;
    }
    fs::rename(newExe, exe, ec);
    if (ec) {
        std::error_code ec2;                        // 新 exe 就位失败 -> 回滚保可用
        fs::rename(oldExe, exe, ec2);
        r.message = "新 exe 就位失败, 已回滚: " + ec.message();
        return r;
    }

    if (progress) progress(100, "完成");
    r.ok = true;
    r.message = "已更新到 v" + targetVersion + ", 重启启动器生效";
    return r;
}

} // namespace hs
