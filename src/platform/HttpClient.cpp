#include "HttpClient.h"

#include <windows.h>
#include <winhttp.h>

#include <sstream>
#include <algorithm>
#include <fstream>

#pragma comment(lib, "winhttp.lib")

namespace hs::net {
namespace {

std::string WideToUtf8(const wchar_t* w) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// URL 解析为 host + path + useTls。支持 http/https。
bool splitUrl(const std::string& url, std::string& scheme, std::string& host,
              std::string& path, int& port) {
    scheme = host = path = "";
    port = 0;
    std::string u = url;
    std::transform(u.begin(), u.end(), u.begin(), ::tolower);

    size_t schemeEnd = u.find("://");
    if (schemeEnd == std::string::npos) return false;
    scheme = u.substr(0, schemeEnd);
    if (scheme != "http" && scheme != "https") return false;

    std::string rest = url.substr(schemeEnd + 3);
    size_t pathStart = rest.find('/');
    std::string authority = (pathStart == std::string::npos) ? rest : rest.substr(0, pathStart);
    path = (pathStart == std::string::npos) ? "/" : rest.substr(pathStart);
    if (path.empty()) path = "/";

    // authority 可能含端口
    size_t colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(']') == std::string::npos) {
        // host:port
        std::string portStr = authority.substr(colon + 1);
        if (!portStr.empty() && std::all_of(portStr.begin(), portStr.end(), ::isdigit)) {
            port = std::atoi(portStr.c_str());
            host = authority.substr(0, colon);
        } else {
            host = authority;
        }
    } else {
        host = authority;
    }
    if (port == 0) port = (scheme == "https") ? 443 : 80;
    return !host.empty();
}

// 读系统代理 (IE 设置)
bool systemProxy(std::string& host, int& port, std::string& bypass) {
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG cfg{};
    if (!WinHttpGetIEProxyConfigForCurrentUser(&cfg)) return false;
    std::string proxyStr;
    if (cfg.lpszProxy) { proxyStr = WideToUtf8(cfg.lpszProxy); }
    if (cfg.lpszProxyBypass) { bypass = WideToUtf8(cfg.lpszProxyBypass); }
    if (cfg.lpszAutoConfigUrl) GlobalFree(cfg.lpszAutoConfigUrl);
    if (cfg.lpszProxy) GlobalFree(cfg.lpszProxy);
    if (cfg.lpszProxyBypass) GlobalFree(cfg.lpszProxyBypass);

    if (proxyStr.empty() || proxyStr == "http://=;") return false;
    // 格式: http=127.0.0.1:7897;https=...; 或 127.0.0.1:7897
    // 取 http= 后的第一个
    std::string httpPart = proxyStr;
    size_t eq = proxyStr.find('=');
    if (eq != std::string::npos) {
        // 形如 http=host:port 或 host:port
        std::string key = proxyStr.substr(0, eq);
        if (key == "http" || key == "https" || key == "socks") {
            size_t semi = proxyStr.find(';', eq);
            httpPart = proxyStr.substr(eq + 1, semi == std::string::npos ? std::string::npos : semi - eq - 1);
        }
    }
    size_t colon = httpPart.rfind(':');
    if (colon == std::string::npos) return false;
    host = httpPart.substr(0, colon);
    port = std::atoi(httpPart.substr(colon + 1).c_str());
    return port > 0;
}

} // namespace

HttpResponse httpGetWithHeaders(const std::string& url,
                                const std::vector<std::pair<std::string,std::string>>& headers,
                                const HttpOptions& opt) {
    HttpResponse r;
    std::string scheme, host, path;
    int port = 0;
    if (!splitUrl(url, scheme, host, path, port)) {
        r.error = "非法 URL: " + url;
        return r;
    }
    bool useTls = (scheme == "https");

    // 代理解析
    std::wstring proxyHostW;
    int proxyPort = 0;
    bool useProxy = false;
    if (opt.proxy.mode == ProxyConfig::Mode::Manual && !opt.proxy.host.empty()) {
        proxyHostW = std::wstring(opt.proxy.host.begin(), opt.proxy.host.end());
        proxyPort = opt.proxy.port;
        useProxy = proxyPort > 0;
    } else if (opt.proxy.mode == ProxyConfig::Mode::System) {
        std::string ph; std::string bp; int pp = 0;
        if (systemProxy(ph, pp, bp)) {
            proxyHostW = std::wstring(ph.begin(), ph.end());
            proxyPort = pp;
            useProxy = true;
        }
    }

    // 创建 session
    HINTERNET hSession = WinHttpOpen(L"AgentDock/1.0",
        useProxy ? WINHTTP_ACCESS_TYPE_NAMED_PROXY : WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        useProxy ? proxyHostW.c_str() : WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { r.error = "WinHttpOpen 失败: " + std::to_string(GetLastError()); return r; }

    std::wstring hostW(host.begin(), host.end());
    std::wstring pathW = std::wstring(path.begin(), path.end());

    HINTERNET hConnect = WinHttpConnect(hSession, hostW.c_str(), (INTERNET_PORT)port, 0);
    if (!hConnect) {
        r.error = "WinHttpConnect 失败: " + std::to_string(GetLastError());
        WinHttpCloseHandle(hSession); return r;
    }

    std::wstring verb = L"GET";
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, verb.c_str(), pathW.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        useTls ? WINHTTP_FLAG_SECURE : 0);
    if (!hRequest) {
        r.error = "WinHttpOpenRequest 失败: " + std::to_string(GetLastError());
        WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return r;
    }

    // 超时
    DWORD timeout = (DWORD)(opt.timeoutSec * 1000);
    WinHttpSetTimeouts(hRequest, timeout, timeout, timeout, timeout);

    // 自定义 headers
    if (!headers.empty()) {
        std::wstring hdrStr;
        for (auto& h : headers) {
            hdrStr += std::wstring(h.first.begin(), h.first.end()) + L": " +
                      std::wstring(h.second.begin(), h.second.end()) + L"\r\n";
        }
        WinHttpAddRequestHeaders(hRequest, hdrStr.c_str(), (DWORD)hdrStr.size(), WINHTTP_ADDREQ_FLAG_REPLACE | WINHTTP_ADDREQ_FLAG_ADD);
    }

    // 可选 TLS 安全选项
    if (useTls && opt.allowInsecureTls) {
        DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                      SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                      SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                      SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));
    }

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        r.error = "WinHttpSendRequest 失败: " + std::to_string(GetLastError());
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return r;
    }
    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        r.error = "WinHttpReceiveResponse 失败: " + std::to_string(GetLastError());
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return r;
    }

    DWORD status = 0; DWORD statusLen = sizeof(status);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusLen, WINHTTP_NO_HEADER_INDEX);
    r.status = status;
    r.ok = (status >= 200 && status < 300);

    // 读 body
    DWORD total = 0;
    std::string body;
    DWORD bytesRead = 0;
    do {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &avail)) break;
        if (avail == 0) break;
        std::vector<char> buf(avail);
        if (!WinHttpReadData(hRequest, buf.data(), avail, &bytesRead)) break;
        if (bytesRead > 0) body.append(buf.data(), bytesRead);
    } while (bytesRead > 0);

    r.body = std::move(body);
    WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
    return r;
}

HttpResponse httpGet(const std::string& url, const HttpOptions& opt) {
    return httpGetWithHeaders(url, {}, opt);
}

DownloadResult downloadFile(const std::string& url, const fs::path& dest,
                            const HttpOptions& opt,
                            std::function<bool(int,std::int64_t,std::int64_t)> progress) {
    DownloadResult r;
    auto resp = httpGetWithHeaders(url, {}, opt);
    // 注意: httpGet 会整包读进内存, 对大文件不理想。此处简单实现; 后续可优化为流式。
    // 先落盘 (内存较大时由调用方控制, M3 组件包多在几百 MB 内可接受)。
    if (!resp.ok) {
        r.error = resp.error.empty() ? ("HTTP " + std::to_string(resp.status)) : resp.error;
        r.status = resp.status;
        return r;
    }
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    {
        std::ofstream out(dest, std::ios::binary);
        if (!out) { r.error = "无法写入文件: " + dest.string(); return r; }
        out.write(resp.body.data(), (std::streamsize)resp.body.size());
    }
    r.ok = true;
    r.status = resp.status;
    r.bytes = resp.body.size();
    r.total = resp.body.size();
    if (progress) progress(100, r.bytes, r.total);
    return r;
}

} // namespace hs::net
