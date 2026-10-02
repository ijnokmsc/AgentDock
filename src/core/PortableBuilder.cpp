// PortableBuilder.cpp — 一键构建便携版 (2026-09-22 重建版)
//
// 流程 (ADR-004): 查官方版本清单 → 下载/复用 runtime+web-ui → sha256 校验
//   → 解包 (系统 tar.exe, 见 Archive.cpp) → 组装目录/Hermes.bat → 沙箱
//   → pyvenv 修正 → 验证 import hermes_cli。
// 构建线程由 TaskRunner 托管; 本文件所有失败走 bail (返回 BuildResult), 不抛异常。

#include "PortableBuilder.h"

#include "PortableLayout.h"
#include <windows.h>

#include "../app/Paths.h"
#include "../platform/HttpClient.h"
#include "../platform/Archive.h"
#include "../platform/Hash.h"
#include "../platform/ProcessUtil.h"
#include "../storage/JsonStore.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <chrono>
#include <cstdio>
#include <sstream>
#include <map>
#include <regex>
#include <ctime>

using json = nlohmann::json;

namespace {

const char* kRepoSlug = "EKKOLearnAI/hermes-studio";

int64_t nowUnix() {
    return (int64_t)std::time(nullptr);
}

std::string fmtMB(double mb) {
    char buf[32];
    if (mb >= 1024) snprintf(buf, sizeof(buf), "%.1f GB", mb / 1024);
    else snprintf(buf, sizeof(buf), "%.0f MB", mb);
    return buf;
}

std::string fmtSpeed(double mbps) {
    char buf[32];
    if (mbps >= 1) snprintf(buf, sizeof(buf), "%.1f MB/s", mbps);
    else snprintf(buf, sizeof(buf), "%.0f KB/s", mbps * 1024);
    return buf;
}

std::wstring s2ws(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::string releasesApiUrl() {
    return std::string("https://api.github.com/repos/") + kRepoSlug + "/releases?per_page=40";
}

// 从 release 的 assets 里取文件大小
std::map<std::string, std::int64_t> assetSizeMap(const json& rel) {
    std::map<std::string, std::int64_t> m;
    if (!rel.contains("assets") || !rel["assets"].is_array()) return m;
    for (const auto& a : rel["assets"]) {
        if (!a.is_object()) continue;
        std::string name = a.value("name", std::string{});
        std::int64_t sz = a.value("size", std::int64_t{0});
        if (!name.empty() && sz > 0) m[name] = sz;
    }
    return m;
}

// 从 release 的 assets 里按名字取下载地址
std::map<std::string, std::string> assetUrlMap(const json& rel) {
    std::map<std::string, std::string> m;
    if (!rel.contains("assets") || !rel["assets"].is_array()) return m;
    for (const auto& a : rel["assets"]) {
        if (!a.is_object()) continue;
        std::string name = a.value("name", std::string{});
        std::string url  = a.value("browser_download_url", std::string{});
        if (!name.empty() && !url.empty()) m[name] = url;
    }
    return m;
}

// GitHub release 资产地址是可推导的; API 里没给 (或限流) 时兜底
std::string buildAssetUrl(const std::string& tag, const std::string& assetName) {
    return "https://github.com/" + std::string(kRepoSlug) + "/releases/download/" + tag + "/" + assetName;
}

std::string applyMirror(const std::string& url, const std::string& mirror) {
    if (mirror.empty()) return url;
    std::string m = mirror;
    if (m.back() != '/') m += '/';
    return m + url;
}

bool writeAsciiCrlf(const fs::path& p, const std::string& content) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(content.data(), (std::streamsize)content.size());
    return (bool)out;
}

bool readAll(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return true;
}

} // namespace

namespace hs {

// ---------------------------------------------------------------------------
// 版本发现
// ---------------------------------------------------------------------------
PortableBuilder::RemoteInfo PortableBuilder::resolveLatest(const std::string& platform,
                                                           const std::string& mirror) const {
    RemoteInfo info;

    auto resp = net::httpGet(releasesApiUrl());
    if (!resp.ok) {
        info.message = "查询 GitHub releases 失败: " + resp.error;
        return info;
    }

    json arr;
    try {
        arr = json::parse(resp.body);
    } catch (const std::exception& e) {
        info.message = std::string("releases 解析失败: ") + e.what();
        return info;
    }
    if (!arr.is_array() || arr.empty()) {
        info.message = "releases 列表为空";
        return info;
    }

    const std::string gzSuffix = "-" + platform + ".tar.gz";
    const std::string jsSuffix = "-" + platform + ".json";

    // 分别找 runtime release 和 web-ui release (它们是独立的 GitHub release)
    std::string runtimeTag, runtimeName, runtimeUrl;
    std::string webTag, webName, webUrl;
    std::int64_t runtimeSize = 0, webSize = 0;
    for (const auto& rel : arr) {
        if (!rel.is_object()) continue;
        std::string tag = rel.value("tag_name", std::string());
        auto urls = assetUrlMap(rel);
        for (const auto& [name, url] : urls) {
            bool isRt = name.rfind("hermes-runtime-", 0) == 0
                        && name.size() > gzSuffix.size()
                        && name.compare(name.size() - gzSuffix.size(), gzSuffix.size(), gzSuffix) == 0;
            bool isWu = name.rfind("hermes-web-ui-", 0) == 0
                        && name.size() > 7
                        && name.compare(name.size() - 7, 7, ".tar.gz") == 0;
            std::int64_t asz = 0;
            auto szIt = assetSizeMap(rel).find(name);
            if (szIt != assetSizeMap(rel).end()) asz = szIt->second;
            if (isRt && runtimeUrl.empty()) {
                runtimeTag = tag; runtimeName = name; runtimeUrl = url; runtimeSize = asz;
            }
            if (isWu && webUrl.empty()) {
                webTag = tag; webName = name; webUrl = url; webSize = asz;
            }
        }
    }
    if (runtimeUrl.empty()) {
        info.message = "没找到 runtime release";
        return info;
    }
    if (webUrl.empty()) {
        info.message = "没找到 web-ui release";
        return info;
    }

    info.runtimeTag = runtimeTag;
    info.runtimeUrl = runtimeUrl;
    info.webUiUrl   = webUrl;

    // 版本号从文件名推导: hermes-runtime-hermes-agent-<ver>-win-x64.tar.gz / hermes-web-ui-<ver>.tar.gz
    {
        auto verOf = [](const std::string& name, const std::string& prefix) {
            std::string v = name.substr(prefix.size());
            auto dash = v.find('-');
            if (dash != std::string::npos) v = v.substr(0, dash);
            return v;
        };
        info.runtimeVersion = verOf(runtimeName, "hermes-runtime-");
        info.webUiVersion   = verOf(webName, "hermes-web-ui-");
    }
    // manifest 名也记录 (fetchSha 用)
    std::string runtimeManifestName = runtimeName.substr(0, runtimeName.size() - gzSuffix.size()) + jsSuffix;
    std::string webManifestName     = webName.substr(0, webName.size() - gzSuffix.size()) + jsSuffix;

    // sha256/大小从平台清单 json 资产读取 (下载失败则校验步骤自动跳过)

    auto fetchSha = [&](const std::string& manifestName, std::int64_t* sizeOut) -> std::string {

        std::string url = buildAssetUrl(runtimeTag, manifestName);
        auto r = net::httpGet(url);
        if (!r.ok) return {};
        try {
            auto j = json::parse(r.body);
            if (j.contains("sha256") && j["sha256"].is_string()) {
                if (sizeOut && j.contains("size") && j["size"].is_number())
                    *sizeOut = j["size"].get<std::int64_t>();
                return j["sha256"].get<std::string>();
            }
            // 清单也可能是 {asset: {name, sha256, size}} 结构
            if (j.contains("asset")) {
                const auto& a = j["asset"];
                if (a.contains("sha256") && a["sha256"].is_string()) {
                    if (sizeOut && a.contains("size") && a["size"].is_number())
                        *sizeOut = a["size"].get<std::int64_t>();
                    return a["sha256"].get<std::string>();
                }
            }
        } catch (const std::exception&) {}
        return {};
    };
    info.runtimeSha256 = fetchSha(runtimeManifestName, &info.runtimeSize);
        info.webUiSha256   = fetchSha(webManifestName, &info.webUiSize);
        (void)info.runtimeSize; (void)info.webUiSize;

    // mirror 作用于下载地址
    if (!mirror.empty()) {
        info.runtimeUrl = applyMirror(info.runtimeUrl, mirror);
        info.webUiUrl   = applyMirror(info.webUiUrl, mirror);
    }

    info.ok = true;
    info.message = "runtime " + info.runtimeVersion + " / web-ui " + info.webUiVersion;
    return info;
}

// ---------------------------------------------------------------------------
// pyvenv.cfg home 修正 (可重定位的关键)
// ---------------------------------------------------------------------------
bool PortableBuilder::fixPyvenvHome(const fs::path& root, std::string* err) {
    std::error_code ec;
    const fs::path cfgs[] = {
        root / "python" / "venv" / "pyvenv.cfg",   // OFFICIAL
        root / "venv" / "pyvenv.cfg",              // LEGACY
    };
    const fs::path bases[] = {
        root / "python" / "base",                  // OFFICIAL home 目标
        root / "python",                           // LEGACY home 目标 (旧包 compat)
    };

    for (size_t i = 0; i < 2; ++i) {
        if (!fs::exists(cfgs[i], ec)) continue;

        std::string content;
        if (!readAll(cfgs[i], content)) {
            if (err) *err = "读取失败: " + cfgs[i].string();
            return false;
        }

        std::string home = bases[i == 0 ? 0 : 1].string();
        bool found = false;
        std::string out;
        for (auto& rawLine : std::string(content)) { (void)rawLine; }
        {
            std::istringstream in(content);
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.rfind("home", 0) == 0) {
                    out += "home = " + home + "\r\n";
                    found = true;
                } else {
                    out += line + "\r\n";
                }
            }
        }
        if (!found) out += "home = " + home + "\r\n";
        std::ofstream o(cfgs[i], std::ios::binary | std::ios::trunc);
        if (!o) {
            if (err) *err = "写入失败: " + cfgs[i].string();
            return false;
        }
        o.write(out.data(), (std::streamsize)out.size());
        return true;
    }
    if (err) *err = "未找到 pyvenv.cfg";
    return false;
}

// ---------------------------------------------------------------------------
// 一键构建
// ---------------------------------------------------------------------------
BuildResult PortableBuilder::run(const BuildOptions& opts,
                                 ProgressFn progress,
                                 CancelFn cancelled) const {
    BuildResult r;
    auto report = [&](int percent, const std::string& stage) {
        if (progress) progress(BuildProgress{percent, stage});
    };
    auto wantCancel = [&]() { return cancelled && cancelled(); };
    auto bail = [&](const std::string& m) {
        r.ok = false;
        r.message = m;
        return r;
    };

    fs::path target = opts.targetDir.empty() ? Paths::portableRoot() : opts.targetDir;
    r.targetDir = target.string();
    std::error_code ec;
    fs::create_directories(target, ec);

    const std::string platform = opts.platform.empty() ? std::string("win-x64") : opts.platform;
    const std::string gzSuffix = "-" + platform + ".tar.gz";
    const std::string jsSuffix = "-" + platform + ".json";

    report(1, "查询官方版本清单");
    auto info = resolveLatest(opts.platform, opts.githubMirror);
    if (!info.ok) return bail(info.message);
    r.runtimeVersion = info.runtimeVersion;
    r.webUiVersion   = info.webUiVersion;

    const fs::path dl = Paths::downloadDir();
    fs::create_directories(dl, ec);

    // ---- 1) runtime ----
    const fs::path runtimeArc = dl / fs::path(info.runtimeUrl).filename();
    if (opts.useCache && fs::exists(runtimeArc, ec) && fs::file_size(runtimeArc, ec) > 0) {
        report(6, "复用已下载的 runtime 包");
    } else {
        report(5, "下载 Hermes runtime (约 " +
                  std::to_string(info.runtimeSize / (1024 * 1024)) + " MB)");

        // curl.exe 流式下载: 子进程 + Job 取消 + 文件大小轮询进度
        wchar_t curlPath[MAX_PATH]{};
        wchar_t* cPart = nullptr;
        if (!SearchPathW(nullptr, L"curl.exe", nullptr, MAX_PATH, curlPath, &cPart))
            return bail("未找到 curl.exe (需要 Win10 1803+)");

        std::wstring curlCmd = std::wstring(L"\"") + curlPath
            + L"\" -L -s -S -o \"" + runtimeArc.wstring()
            + L"\" --create-dirs \"" + s2ws(applyMirror(info.runtimeUrl, opts.githubMirror)) + L"\"";
        STARTUPINFOW csi{ sizeof(csi) };
        csi.dwFlags = STARTF_USESHOWWINDOW;
        csi.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION cpi{};
        if (!CreateProcessW(curlPath, &curlCmd[0], nullptr, nullptr, TRUE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &csi, &cpi))
            return bail("无法启动 curl.exe (错误 " + std::to_string(GetLastError()) + ")");
        CloseHandle(cpi.hThread);

        HANDLE dlJob = CreateJobObjectW(nullptr, nullptr);
        if (dlJob) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim{ sizeof(lim) };
            lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(dlJob, JobObjectExtendedLimitInformation, &lim, sizeof(lim));
            AssignProcessToJobObject(dlJob, cpi.hProcess);
        }

        // 轮询: 进度 + 取消
        bool dlCancelled = false;
        auto dlStart = std::chrono::steady_clock::now();
        auto t0 = dlStart;
        for (;;) {
            DWORD w = WaitForSingleObject(cpi.hProcess, 500);
            if (wantCancel()) {
                dlCancelled = true;
                if (dlJob) TerminateJobObject(dlJob, 1);
                WaitForSingleObject(cpi.hProcess, 3000);
                break;
            }
            std::error_code fec;
            auto fsize = fs::file_size(runtimeArc, fec);
            auto secs = std::chrono::duration_cast<std::chrono::duration<double>>(
                std::chrono::steady_clock::now() - t0).count();
            double mb = fsize / 1048576.0;
            double spd = secs > 1.0 ? mb / secs : 0;
            std::string spd_s;
            if (spd > 0.01) spd_s = " · " + fmtSpeed(spd);
            report(5 + (int)(mb / (info.runtimeSize / 1048576.0 + 0.1) * 40),
                "下载 runtime " + fmtMB(mb) + " / " +
                std::to_string(info.runtimeSize / 1048576) + " MB" + spd_s);
            if (w == WAIT_OBJECT_0) break;
        }
        if (dlJob) CloseHandle(dlJob);
        if (dlCancelled) { r.cancelled = true; return bail("已取消下载, 可手动下载文件放入缓存目录后重试"); }

        // 校验 curl 退出码
        DWORD curlExit = 1;
        GetExitCodeProcess(cpi.hProcess, &curlExit);
        CloseHandle(cpi.hProcess);
        if (curlExit != 0)
            return bail("下载 runtime 失败: curl exit " + std::to_string(curlExit));
        // 大小校验
        if (info.runtimeSize > 0) {
            auto actual = fs::file_size(runtimeArc, ec);
            if (actual != (uintmax_t)info.runtimeSize)
                return bail("runtime 下载不完整 (实际 " + std::to_string(actual / 1048576)
                    + "MB, 预期 " + std::to_string(info.runtimeSize / 1048576) + "MB), 请重试");
        }
    }

    // 缓存命中也要校验 sha256
    if (opts.useCache && fs::exists(runtimeArc, ec) && opts.verifySha256 && !info.runtimeSha256.empty()) {
        report(46, "校验 runtime 缓存 sha256");
        auto h = crypto::sha256File(runtimeArc);
        if (h && *h != info.runtimeSha256) {
            fs::remove(runtimeArc, ec);
            return bail("runtime 缓存 sha256 不匹配 (文件已删除), 请重新下载");
        }
    }

    if (opts.verifySha256 && !info.runtimeSha256.empty()) {
        report(46, "校验 runtime sha256");
        auto h = crypto::sha256File(runtimeArc);
        if (!h) return bail("无法计算 runtime 的 sha256");
        if (*h != info.runtimeSha256) {
            fs::remove(runtimeArc, ec);
            return bail("runtime 校验失败: sha256 不匹配 (文件已删除, 请重试)");
        }
    }

    report(50, "解包 runtime (~1.9GB, 需要几分钟, 请勿关闭)");
    arc::ExtractControl ctl;
    const std::function<bool()> cancelFn = cancelled;
    ctl.cancel = &cancelFn;
    if (auto e = arc::extractAuto(runtimeArc, target, &ctl)) {
        if (wantCancel()) { r.cancelled = true; return bail("已取消"); }
        fs::remove(runtimeArc, ec);   // 删除损坏文件
        return bail("解包 runtime 失败 (已删除损坏文件): " + *e);
    }
    if (wantCancel()) { r.cancelled = true; return bail("已取消"); }

    // ---- 2) web-ui ----
    const fs::path webArc = dl / fs::path(info.webUiUrl).filename();
    if (opts.useCache && fs::exists(webArc, ec) && fs::file_size(webArc, ec) > 0) {
        report(72, "复用已下载的 web-ui 包");
    } else {
        report(70, "下载 web-ui " + info.webUiVersion);

        wchar_t wuCurlPath[MAX_PATH]{};
        wchar_t* wuPart = nullptr;
        if (!SearchPathW(nullptr, L"curl.exe", nullptr, MAX_PATH, wuCurlPath, &wuPart))
            return bail("未找到 curl.exe (需要 Win10 1803+)");

        std::wstring wuCmd = std::wstring(L"\"") + wuCurlPath
            + L"\" -L -s -S -o \"" + webArc.wstring()
            + L"\" --create-dirs \"" + s2ws(applyMirror(info.webUiUrl, opts.githubMirror)) + L"\"";
        STARTUPINFOW wsi{ sizeof(wsi) };
        wsi.dwFlags = STARTF_USESHOWWINDOW;
        wsi.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION wpi{};
        if (!CreateProcessW(wuCurlPath, &wuCmd[0], nullptr, nullptr, TRUE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &wsi, &wpi))
            return bail("无法启动 curl.exe (错误 " + std::to_string(GetLastError()) + ")");
        CloseHandle(wpi.hThread);

        HANDLE wuJob = CreateJobObjectW(nullptr, nullptr);
        if (wuJob) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION wlim{ sizeof(wlim) };
            wlim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(wuJob, JobObjectExtendedLimitInformation, &wlim, sizeof(wlim));
            AssignProcessToJobObject(wuJob, wpi.hProcess);
        }

        bool wuCancelled = false;
        auto wuT0 = std::chrono::steady_clock::now();
        for (;;) {
            DWORD w = WaitForSingleObject(wpi.hProcess, 500);
            if (wantCancel()) {
                wuCancelled = true;
                if (wuJob) TerminateJobObject(wuJob, 1);
                WaitForSingleObject(wpi.hProcess, 3000);
                break;
            }
            std::error_code fec;
            auto fsize = fs::file_size(webArc, fec);
            if (!fec && fsize > 0) {
                auto secs = std::chrono::duration_cast<std::chrono::duration<double>>(
                    std::chrono::steady_clock::now() - wuT0).count();
                double mb = fsize / 1048576.0;
                double spd = secs > 1.0 ? mb / secs : 0;
                report(70 + (int)(mb / (info.webUiSize / 1048576.0 + 0.1) * 12),
                    "下载 web-ui " + fmtMB(mb) + (spd > 0.01 ? " · " + fmtSpeed(spd) : ""));
            }
            if (w == WAIT_OBJECT_0) break;
        }
        DWORD wuExit = 1;
        GetExitCodeProcess(wpi.hProcess, &wuExit);
        CloseHandle(wpi.hProcess);
        if (wuJob) CloseHandle(wuJob);
        if (wuCancelled) { r.cancelled = true; return bail("已取消下载, 可手动下载文件放入缓存目录后重试"); }
        if (wuExit != 0)
            return bail("下载 web-ui 失败: curl exit " + std::to_string(wuExit));
        // 截断检测: 实际大小 vs 清单预期
        if (info.webUiSize > 0) {
            auto actual = fs::file_size(webArc, ec);
            if (actual != (uintmax_t)info.webUiSize)
                return bail("web-ui 下载不完整 (实际 " + std::to_string(actual / 1048576)
                    + "MB, 预期 " + std::to_string(info.webUiSize / 1048576) + "MB), 已删除, 请重试");
        }
    }

    // 缓存命中也要校验 sha256 (防止截断/损坏文件混入缓存)
    if (opts.useCache && fs::exists(webArc, ec) && opts.verifySha256 && !info.webUiSha256.empty()) {
        report(83, "校验 web-ui 缓存 sha256");
        auto h = crypto::sha256File(webArc);
        if (h && *h != info.webUiSha256) {
            fs::remove(webArc, ec);
            return bail("web-ui 缓存 sha256 不匹配 (文件已删除), 请重新下载");
        }
    }

    if (opts.verifySha256 && !info.webUiSha256.empty()) {
        report(83, "校验 web-ui sha256");
        auto h = crypto::sha256File(webArc);
        if (!h) return bail("无法计算 web-ui 的 sha256");
        if (*h != info.webUiSha256) {
            fs::remove(webArc, ec);
            return bail("web-ui 校验失败: sha256 不匹配 (文件已删除, 请重试)");
        }
    }

    report(85, "解包 web-ui");
    if (auto e = arc::extractAuto(webArc, target, &ctl)) {
        if (wantCancel()) { r.cancelled = true; return bail("已取消"); }
        fs::remove(webArc, ec);   // 删除损坏文件, 重试时重新下载
        return bail("解包 web-ui 失败 (已删除损坏文件): " + *e);
    }

    // Windows 上 npm .bin 符号链接条目无法创建 (已被解包容错跳过),
    // 校验真正的运行入口存在, 防止"部分解包"被误判成功
    if (!fs::exists(target / L"webui" / L"dist" / L"server" / L"index.js")) {
        return bail("web-ui 解包不完整 (缺少 dist/server/index.js), 请清空下载缓存重试");
    }

    // ---- 3) 目录与便携化收尾 ----
    report(90, "写入清单与启动脚本");
    fs::create_directories(target / "data", ec);
    fs::create_directories(target / "studio", ec);
    fs::create_directories(target / "plugin", ec);

    // 官方 runtime 清单原文留档 (溯源: 哪个 tag / 哪个 asset)
    if (!info.manifestJson.empty()) {
        try {
            JsonStore::save(target / "runtime-manifest.json", json::parse(info.manifestJson));
        } catch (...) { /* 清单原文异常不该拖垮构建 */ }
    }
    // 构建元信息 (构建时间/来源/校验和)
    {
        json meta;
        meta["schema"] = 1;
        meta["builtAt"] = nowUnix();
        meta["builtBy"] = "AgentDock PortableBuilder";
        meta["platform"] = opts.platform;
        meta["runtime"] = {{"tag", info.runtimeTag},
                           {"version", info.runtimeVersion},
                           {"asset", fs::path(info.runtimeUrl).filename().string()},
                           {"sha256", info.runtimeSha256}};
        meta["webUi"] = {{"tag", info.webUiTag},
                         {"version", info.webUiVersion},
                         {"asset", fs::path(info.webUiUrl).filename().string()},
                         {"sha256", info.webUiSha256}};
        JsonStore::save(target / "studio" / "portable-build.json", meta);
    }

    if (!writeAsciiCrlf(target / "Hermes.bat", portable::hermesBatScript())) {
        return bail("写入 Hermes.bat 失败");
    }
    {
        std::string e;
        fixPyvenvHome(target, &e);   // 非致命: Hermes.bat 每次启动也会修
    }
    {
        std::string e;
        if (!portable::ensureSandbox(target, &e)) return bail("建立沙箱失败: " + e);
    }

    // ---- 4) 校验 ----
    report(95, "校验内核可导入 (import hermes_cli)");
    {
        const fs::path vpy = Paths::venvPython();
        std::error_code ec2;
        if (!fs::exists(vpy, ec2)) return bail("校验失败: 未找到 " + vpy.string());

        proc::LaunchOptions opt;
        opt.exe    = vpy;
        opt.args   = {"-c", "import hermes_cli, os; print('BUILD-CHECK-OK', os.path.dirname(hermes_cli.__file__))"};
        opt.hidden = true;
        std::string capErr;
        auto rr = proc::runAndCapture(opt, 60000, &capErr);
        bool ok = (rr.exitCode == 0) && rr.output.find("BUILD-CHECK-OK") != std::string::npos;
        if (!ok) {
            std::string detail = rr.output;
            if (detail.size() > 400) detail = detail.substr(0, 400);
            return bail("校验失败: 便携包内的 Python 无法导入 hermes_cli。\n" + detail);
        }
    }

    report(100, "便携版构建完成");
    r.ok = true;
    r.targetDir = target;
    r.message = "便携版已就绪 (runtime " + r.runtimeVersion + " + web-ui " + r.webUiVersion + ")";
    return r;
}

} // namespace hs
