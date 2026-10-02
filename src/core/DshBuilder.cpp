#include "DshBuilder.h"
#include "DshRuntime.h"
#include "UpdateManager.h"
#include "../app/Paths.h"
#include "../platform/HttpClient.h"
#include "../platform/Archive.h"
#include "../platform/Hash.h"
#include "../platform/ProcessUtil.h"

#include <nlohmann/json.hpp>
#include <chrono>
#include <cstring>
#include <fstream>
#include <regex>
#include <system_error>

namespace hs::dsh {

namespace fs2 = std::filesystem;
using nlohmann::json;

namespace {

bool cancelledDefault(const std::function<bool()>& c) { return c && c(); }

bool httpDownloadToFile(const std::string& url, const fs2::path& dest, std::string* err) {
    auto dl = net::downloadFile(url, dest, {});
    if (!dl.ok) { if (err) *err = "下载失败: " + dl.error + " (" + url + ")"; return false; }
    return true;
}

// Dsh.bat 模板 —— 与 DshRuntime::pinnedEnv 同一份环境事实。
// 纯 ASCII (cmd 按 OEM 码页解析 bat, 非 ASCII 注释会破坏后续解析) + CRLF。
constexpr const char* kDshBat =
"@echo off\r\n"
"setlocal EnableExtensions\r\n"
"set \"HERE=%~dp0\"\r\n"
"if \"%HERE:~-1%\"==\"\\\" set \"HERE=%HERE:~0,-1%\"\r\n"
"\r\n"
"rem ===== portable environment pinning: keep everything inside this folder =====\r\n"
"set \"DSH_HOME=%HERE%\\data\\dsh\"\r\n"
"set \"HOME=%HERE%\\_home\"\r\n"
"set \"USERPROFILE=%HERE%\\_home\"\r\n"
"set \"APPDATA=%HERE%\\_home\\AppData\\Roaming\"\r\n"
"set \"LOCALAPPDATA=%HERE%\\_home\\AppData\\Local\"\r\n"
"set \"TEMP=%HERE%\\_home\\Temp\"\r\n"
"set \"TMP=%HERE%\\_home\\Temp\"\r\n"
"set \"NPM_CONFIG_CACHE=%HERE%\\studio\\cache\\npm-cache\"\r\n"
"set \"PNPM_HOME=%HERE%\\studio\\cache\\pnpm\"\r\n"
"set \"PATH=%HERE%\\node;%PATH%\"\r\n"
"\r\n"
"set \"NODE_EXE=%HERE%\\node\\node.exe\"\r\n"
"set \"DSH_BIN=%HERE%\\dsh\\node_modules\\@deepseek-ai\\dsh\\lib\\bin.js\"\r\n"
"if not exist \"%NODE_EXE%\" (\r\n"
"    echo [dsh] node.exe not found under %HERE%\\node\r\n"
"    exit /b 1\r\n"
")\r\n"
"if not exist \"%DSH_BIN%\" (\r\n"
"    echo [dsh] dsh not installed under %HERE%\\dsh\r\n"
"    exit /b 1\r\n"
")\r\n"
"\r\n"
"\"%NODE_EXE%\" \"%DSH_BIN%\" %*\r\n";

std::string runCapture(const proc::LaunchOptions& opt, int timeoutMs, int* code = nullptr) {
    auto r = proc::runAndCapture(opt, timeoutMs);
    if (code) *code = r.exitCode;
    return r.output;
}

// npm install @deepseek-ai/dsh@<ver> 到 <target>/dsh (先清 node_modules, 与构建同语义)
std::string npmInstallDsh(const fs2::path& target, const std::string& ver, int* code) {
    fs2::path nodeDir = target / L"node";
    fs2::path dshDir = target / L"dsh";
    fs2::path npmCache = target / L"studio" / L"cache" / L"npm-cache";
    fs2::create_directories(npmCache, std::error_code());
    fs2::path isolatedRc = target / L"studio" / L"cache" / L"npmrc";
    { std::ofstream rc(isolatedRc, std::ios::binary | std::ios::trunc); }
    proc::LaunchOptions opt;
    opt.exe        = nodeDir / L"node.exe";
    opt.args       = { (nodeDir / L"node_modules" / L"npm" / L"bin" / L"npm-cli.js").string(),
                       "install", "@deepseek-ai/dsh@" + ver, "--loglevel=error" };
    opt.workingDir = dshDir;
    opt.hidden     = true;
    opt.env        = {
        { "NPM_CONFIG_CACHE",           npmCache.string() },
        { "NPM_CONFIG_USERCONFIG",      isolatedRc.string() },
        { "NPM_CONFIG_UPDATE_NOTIFIER", "false" },
        { "NPM_CONFIG_AUDIT",           "false" },
        { "NPM_CONFIG_FUND",            "false" },
    };
    // POSIX test 垫片前插 PATH (web-ui 同因): 包 prepare 里的 "[ -d x ]" 在 cmd 下
    // 无法解析会误触发需 devDeps 的构建
    if (fs2::path shim = Paths::ensurePosixTestShimDir(); !shim.empty()) {
        const char* curPath = std::getenv("PATH");
        opt.env.push_back({ "PATH", shim.string() + ";" + (curPath ? curPath : "") });
    }
    return runCapture(opt, 15 * 60 * 1000, code);
}

// 下载 (缓存+sha256) 并解包归位 <target>/node。返回错误串 (空 = 成功)。
// 已存在 node/ 时由调用方自行备份/删除 —— 本函数直接覆盖目标。
std::string fetchAndPlaceNode(const fs2::path& target, const std::string& ver,
                              bool useCache,
                              const std::function<void(const BuildProgress&)>& progress,
                              const std::function<bool()>& cancelled) {
    std::error_code ec;
    const std::string vtag = ver[0] == 'v' ? ver : "v" + ver;
    const std::string vbare = vtag.substr(1);
    fs2::path cache = target / L"studio" / L"cache" / L"downloads";
    fs2::path zip = cache / ("node-" + vtag + "-win-x64.zip");
    fs2::path shasums = cache / ("SHASUMS256-" + vtag + ".txt");
    fs2::create_directories(cache, ec);
    const std::string base = "https://nodejs.org/dist/" + vtag;

    if (useCache && fs2::exists(zip, ec)) {
        if (progress) progress({ 3, "\xe4\xbd\xbf\xe7\x94\xa8\xe7\xbc\x93\xe5\xad\x98\xe7\x9a\x84 node zip" });
    } else {
        if (progress) progress({ 2, "\xe4\xb8\x8b\xe8\xbd\xbd Node " + vtag + " (~30MB)" });
        std::string dlErr;
        if (!httpDownloadToFile(base + "/node-" + vtag + "-win-x64.zip", zip, &dlErr)) return dlErr;
    }
    if (!fs2::exists(shasums, ec)) {
        std::string dlErr;
        if (!httpDownloadToFile(base + "/SHASUMS256.txt", shasums, &dlErr))
            return "SHASUMS256.txt " + dlErr;
    }
    {
        std::ifstream in(shasums);
        std::string line, expect;
        const std::string needle = "node-" + vtag + "-win-x64.zip";
        while (std::getline(in, line))
            if (line.size() > 64 && line.find(needle) != std::string::npos) { expect = line.substr(0, 64); break; }
        auto actual = crypto::sha256File(zip);
        if (!actual || expect.empty() || *actual != expect)
            return "node zip sha256 \xe6\xa0\xa1\xe9\xaa\x8c\xe5\xa4\xb1\xe8\xb4\xa5 (\xe5\x88\xa0\xe7\xbc\x93\xe5\xad\x98\xe6\x96\x87\xe4\xbb\xb6\xe9\x87\x8d\xe8\xaf\x95)";
        if (progress) progress({ 8, "node zip sha256 \xe6\xa0\xa1\xe9\xaa\x8c\xe9\x80\x9a\xe8\xbf\x87" });
    }

    fs2::path nodeDir = target / L"node";
    fs2::path tmp = cache / (L"node_tmp_" + std::wstring(vtag.begin(), vtag.end()));
    fs2::remove_all(tmp, ec);
    if (progress) progress({ 12, "\xe8\xa7\xa3\xe5\x8c\x85 Node" });
    if (auto arcErr = arc::extractAuto(zip, tmp))
        return "node \xe8\xa7\xa3\xe5\x8c\x85\xe5\xa4\xb1\xe8\xb4\xa5: " + *arcErr;
    fs2::path srcTree;
    for (fs2::directory_iterator it(tmp, ec), end; it != end; ++it)
        if (it->is_directory(ec)) { srcTree = it->path(); break; }
    if (srcTree.empty() || !fs2::exists(srcTree / L"node.exe", ec))
        return "node \xe5\x8c\x85\xe7\xbb\x93\xe6\x9e\x84\xe5\xbc\x82\xe5\xb8\xb8 (\xe7\xbc\xba node.exe)";
    fs2::remove_all(nodeDir, ec);
    ec.clear();
    fs2::rename(srcTree, nodeDir, ec);
    if (ec) {
        ec.clear();
        fs2::create_directories(nodeDir, ec);
        fs2::copy(srcTree, nodeDir, fs2::copy_options::recursive | fs2::copy_options::overwrite_existing, ec);
    }
    if (ec) return "node \xe5\xbd\x92\xe4\xbd\x8d\xe5\xa4\xb1\xe8\xb4\xa5: " + ec.message();
    fs2::remove_all(tmp, ec);
    if (progress) progress({ 25, "Node " + vtag + " \xe5\xb0\xb1\xe7\xbb\xaa" });
    return {};
}

} // namespace

RemoteInfo resolveLatest() {
    RemoteInfo info;
    // npm packument: dist-tags.latest + versions 全列表
    // 注意 %2F: 包名里的 '/' 在 registry 路径里必须编码
    auto npmResp = net::httpGet("https://registry.npmjs.org/@deepseek-ai%2Fdsh");
    if (npmResp.ok) {
        try {
            auto j = json::parse(npmResp.body);
            if (j.contains("dist-tags") && j["dist-tags"].contains("latest"))
                info.dshLatest = j["dist-tags"]["latest"].get<std::string>();
            if (j.contains("versions") && j["versions"].is_object()) {
                for (auto it = j["versions"].begin(); it != j["versions"].end(); ++it)
                    info.dshVersions.push_back(it.key());
            }
        } catch (const std::exception& e) {
            info.message = std::string("npm registry 响应解析失败: ") + e.what();
        }
    } else {
        info.message = "npm registry 不可达: " + npmResp.error;
    }

    auto nodeResp = net::httpGet("https://nodejs.org/dist/index.json");
    if (nodeResp.ok) {
        try {
            auto arr = json::parse(nodeResp.body);
            if (arr.is_array()) {
                for (auto& v : arr) {                       // 官方已按新->旧排序
                    if (v.contains("lts") && !v["lts"].is_null() && v["lts"] != false
                        && v.contains("version")) {
                        info.nodeLts = v["version"].get<std::string>();
                        break;
                    }
                }
            }
        } catch (...) { /* nodeLts 缺席时用 opts 默认值 */ }
    }

    info.ok = !info.dshLatest.empty() && !info.nodeLts.empty();
    if (info.ok) info.message = "dsh " + info.dshLatest + " / node LTS " + info.nodeLts;
    else if (info.message.empty()) info.message = "版本源响应不完整";
    return info;
}

BuildResult run(const BuildOptions& opts,
                std::function<void(const BuildProgress&)> progress,
                std::function<bool()> cancelled) {
    BuildResult r;
    fs2::path target = opts.targetDir.empty() ? fs2::path(L"DSH") : opts.targetDir;
    if (target.is_relative()) target = fs2::current_path() / target;
    std::error_code ec;

    auto prog = [&](int pct, const std::string& stage) {
        if (progress) progress({ pct, stage });
    };
    auto fail = [&](const std::string& msg) {
        r.message = msg;
        return r;
    };

    const std::string ver = opts.nodeVersion[0] == 'v' ? opts.nodeVersion : "v" + opts.nodeVersion;

    // ---- 1-2. node zip (缓存 -> 校验 -> 下载 -> 解包归位) ----
    if (cancelledDefault(cancelled)) { r.cancelled = true; r.message = "已取消"; return r; }
    {
        std::string nerr = fetchAndPlaceNode(target, ver, opts.useCache, progress, cancelled);
        if (!nerr.empty()) return fail(nerr);
    }

    // ---- 3. npm install @deepseek-ai/dsh@<ver> (包内 npm, 隔离环境) ----
    if (cancelledDefault(cancelled)) { r.cancelled = true; r.message = "已取消"; return r; }
    fs2::path nodeDir = target / L"node";
    fs2::path dshDir = target / L"dsh";
    fs2::path npmCli = nodeDir / L"node_modules" / L"npm" / L"bin" / L"npm-cli.js";
    if (!fs2::exists(npmCli, ec)) return fail("node 包缺 npm (npm-cli.js)");
    fs2::remove_all(dshDir / L"node_modules", ec);
    fs2::create_directories(dshDir / L"node_modules", ec);
    prog(30, "npm install @deepseek-ai/dsh@" + opts.dshVersion + " (522 包, 约 2-5 分钟)");
    int code = 0;
    auto out = npmInstallDsh(target, opts.dshVersion, &code);   // 阶段内不可取消
    if (code != 0)
        return fail("npm install 失败 (exit " + std::to_string(code) + "): " + out.substr(out.size() > 800 ? out.size() - 800 : 0));
    fs2::path dshBin = dshDir / L"node_modules" / L"@deepseek-ai" / L"dsh" / L"lib" / L"bin.js";
    if (!fs2::exists(dshBin, ec)) return fail("npm install 后 bin.js 缺失");
    prog(80, "dsh " + opts.dshVersion + " 安装完成");

    // ---- 4-5. Dsh.bat + 目录 ----
    { std::ofstream bat(target / L"Dsh.bat", std::ios::binary | std::ios::trunc);
      bat.write(kDshBat, (std::streamsize)strlen(kDshBat)); }
    dsh::ensureDirsAt(target);
    prog(85, "Dsh.bat + 目录就绪");

    // ---- 6. 预初始化 profiles/web (首启零网络) ----
    if (cancelledDefault(cancelled)) { r.cancelled = true; r.message = "已取消"; return r; }
    proc::LaunchOptions initOpt;
    initOpt.exe        = nodeDir / L"node.exe";
    initOpt.args       = { dshBin.string(), "--dump-default-config", "--profile", "web" };
    initOpt.workingDir = target;
    initOpt.hidden     = true;
    initOpt.env        = dsh::pinnedEnvAt(target);
    code = 0;
    out = runCapture(initOpt, 120000, &code);
    if (code != 0) return fail("profile 预初始化失败 (exit " + std::to_string(code) + "): " + out.substr(0, 400));
    prog(90, "profiles/web 预初始化完成");

    // ---- 7. runtime-manifest.json ----
    {
        fs2::path zip = target / L"studio" / L"cache" / L"downloads" /
                        ("node-" + ver + "-win-x64.zip");
        auto sha = crypto::sha256File(zip);
        json manifest = {
            { "schema", 1 },
            { "product", "dsh-portable" },
            { "builtAt", "" },
            { "components", {
                { "node", { { "version", ver }, { "sha256", sha ? *sha : "" } } },
                { "dsh",  { { "version", opts.dshVersion } } },
            } },
        };
        std::ofstream mf(target / L"runtime-manifest.json", std::ios::binary | std::ios::trunc);
        mf << manifest.dump(2) << "\n";
    }

    // ---- 8. 校验: dsh --version + Dsh.bat --version ----
    proc::LaunchOptions vopt;
    vopt.exe        = nodeDir / L"node.exe";
    vopt.args       = { dshBin.string(), "--version" };
    vopt.workingDir = target;
    vopt.env        = dsh::pinnedEnvAt(target);
    code = 0;
    out = runCapture(vopt, 30000, &code);
    if (code != 0 || out.find(opts.dshVersion) == std::string::npos)
        return fail("构建后校验失败: dsh --version = " + out);
    prog(100, "校验通过");
    r.ok = true;
    r.message = "DSH 便携包构建完成: node " + ver + " / dsh " + opts.dshVersion;
    return r;
}

BuildResult updateCore(const std::string& version,
                       std::function<void(const BuildProgress&)> progress) {
    BuildResult r;
    fs2::path target = root();
    std::error_code ec;
    if (!installed()) { r.message = "DSH 未安装"; return r; }
    auto prog = [&](int pct, const std::string& stage) { if (progress) progress({ pct, stage }); };

    UpdateManager um;
    fs2::path dshDir = target / L"dsh";
    // 备份阶段进度 (节流 200ms, 上报已复制 MB) —— dsh 目录数百 MB, 备份耗时 1-2 分钟
    auto lastTick = std::chrono::steady_clock::now();
    auto backupProg = [&](std::uintmax_t bytes) {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTick).count() < 200) return;
        lastTick = now;
        prog(4, "备份当前版本: 已复制 " + std::to_string(bytes / (1024 * 1024)) + " MB ...");
    };
    // 备份 tag 语义 = 被备份的(当前)版本 (读 dsh 包自身 package.json), 不是目标版本
    std::string curDsh = "unknown";
    {
        std::ifstream pf(target / L"dsh" / L"node_modules" / L"@deepseek-ai" / L"dsh" / L"package.json");
        if (pf) {
            try {
                auto pj = json::parse(pf);
                if (pj.contains("version")) curDsh = pj["version"].get<std::string>();
            } catch (...) {}
        }
    }
    auto backup = um.backupComponent(dshDir, "dsh", "pre-" + curDsh, nullptr, backupProg);
    if (backup.empty()) { r.message = "备份 dsh 目录失败"; return r; }

    fs2::remove_all(dshDir / L"node_modules", ec);
    fs2::create_directories(dshDir / L"node_modules", ec);
    prog(10, "npm install @deepseek-ai/dsh@" + version);
    int code = 0;
    auto out = npmInstallDsh(target, version, &code);
    fs2::path dshBin = dshDir / L"node_modules" / L"@deepseek-ai" / L"dsh" / L"lib" / L"bin.js";
    proc::LaunchOptions vopt;
    vopt.exe        = nodeExe();
    vopt.args       = { dshBin.string(), "--version" };
    vopt.workingDir = target;
    vopt.env        = pinnedEnvAt(target);
    code = 0;
    out = runCapture(vopt, 30000, &code);
    bool okVer = code == 0 && out.find(version) != std::string::npos;
    if (!fs2::exists(dshBin, ec) || !okVer) {
        if (um.rollback(backup))
            r.message = "更新失败, 已回滚到原版本";
        else
            r.message = "更新失败且回滚失败 (备份: " + backup.string() + ")";
        return r;
    }
    prog(100, "完成");
    r.ok = true;
    r.message = "dsh 已更新到 " + version;
    return r;
}

BuildResult updateNode(const std::string& version,
                       std::function<void(const BuildProgress&)> progress) {
    BuildResult r;
    fs2::path target = root();
    std::error_code ec;
    if (!installed()) { r.message = "DSH 未安装"; return r; }
    const std::string vtag = version[0] == 'v' ? version : "v" + version;

    UpdateManager um;
    auto lastTick = std::chrono::steady_clock::now();
    auto backupProg = [&](std::uintmax_t bytes) {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTick).count() < 200) return;
        lastTick = now;
        if (progress) progress({ 1, "备份当前 Node: 已复制 " + std::to_string(bytes / (1024 * 1024)) + " MB ..." });
    };
    // 备份 tag 语义 = 被备份的(当前)版本, 不是目标版本
    std::string curNode = "unknown";
    {
        auto nvCur = proc::runAndCapture({ nodeExe(), { "--version" } }, 15000);
        std::string got = nvCur.output;
        while (!got.empty() && (got.back() == '\n' || got.back() == '\r')) got.pop_back();
        if (nvCur.exitCode == 0 && !got.empty()) curNode = got;
    }
    auto backup = um.backupComponent(target / L"node", "dshnode", "pre-" + curNode, nullptr, backupProg);
    if (backup.empty()) { r.message = "备份 node 目录失败"; return r; }

    // 下载解包覆盖 node/ (备份已留); 失败回滚
    auto prog2 = [&](const BuildProgress& p) { if (progress) progress(p); };
    auto noopCancel = []() { return false; };
    {
        std::string nerr = fetchAndPlaceNode(target, vtag, true, prog2, noopCancel);
        if (!nerr.empty()) {
            if (um.rollback(backup))
                r.message = "更新失败, 已回滚: " + nerr;
            else
                r.message = "更新失败且回滚失败 (备份: " + backup.string() + "): " + nerr;
            return r;
        }
    }
    // 校验 node 可执行
    auto nv = proc::runAndCapture({ nodeExe(), { "--version" } }, 15000);
    std::string got = nv.output;
    while (!got.empty() && (got.back() == '\n' || got.back() == '\r')) got.pop_back();
    if (nv.exitCode != 0 || got != vtag) {
        if (um.rollback(backup))
            r.message = "更新后校验失败 (" + got + "), 已回滚";
        else
            r.message = "更新后校验失败且回滚失败";
        return r;
    }
    if (progress) progress({ 100, "完成" });
    r.ok = true;
    r.message = "DSH Node 已更新到 " + vtag;
    return r;
}

} // namespace hs::dsh
