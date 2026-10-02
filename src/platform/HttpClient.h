#pragma once
// 极简 HTTP 客户端 (WinHTTP 封装)。
//
// 为 M3 更新中心服务: GET / 下载到文件 / 支持代理三态 / 超时 / 进度回调。
// 用 WinHTTP 而非 libcurl, 因为便携零依赖 + 支持系统代理 + 不引入第三方 DLL。
//
// 代理三态 (对应 NetSettings::ProxyMode):
//   System  读 Windows IE 代理设置 (注册表 Internet Settings)
//   Direct  直连, 忽略代理
//   Manual  手动指定 host:port
//
// 线程安全: 每个请求独立 WinHTTP session, 可并发; 不可跨线程复用。

#include <string>
#include <optional>
#include <functional>
#include <vector>
#include <cstdint>
#include <filesystem>

namespace fs = std::filesystem;

#ifndef HERMESSTUDIO_VERSION
#define HERMESSTUDIO_VERSION "0.0.0"
#endif

namespace hs::net {

// 代理配置
struct ProxyConfig {
    enum class Mode { System, Direct, Manual };
    Mode   mode = Mode::System;
    std::string host;     // Manual 时使用
    int    port = 0;      // Manual 时使用
    std::string username;
    std::string password;
    bool   socks5 = false;   // 暂不支持 SOCKS, 仅 HTTP/HTTPS 代理
};

struct HttpOptions {
    ProxyConfig proxy;
    int  timeoutSec = 30;       // 连接/读超时
    int  maxRedirects = 5;
    bool allowInsecureTls = false;   // 忽略证书错误 (默认 false)
    std::string userAgent = "AgentDock/" HERMESSTUDIO_VERSION;
};

struct HttpResponse {
    bool        ok = false;
    long        status = 0;
    std::string body;        // GET 到内存时
    std::string error;       // 失败原因
    std::string finalUrl;    // 重定向后的最终 URL
};

// 请求 HEAD/GET 到内存。返回是否成功。
HttpResponse httpGet(const std::string& url, const HttpOptions& opt = {});

// 请求并附带自定义 header (如 Accept)。GET 到内存。
HttpResponse httpGetWithHeaders(const std::string& url,
                                const std::vector<std::pair<std::string,std::string>>& headers,
                                const HttpOptions& opt = {});

// 下载到文件。progress: percent(0-100) 或 -1(未知), 返回 false 可取消。
struct DownloadResult {
    bool        ok = false;
    long        status = 0;
    std::string error;
    std::int64_t bytes = 0;
    std::int64_t total = 0;   // 可能未知 (-1)
};
DownloadResult downloadFile(const std::string& url, const fs::path& dest,
                            const HttpOptions& opt = {},
                            std::function<bool(int percent, std::int64_t done, std::int64_t total)> progress = {});

} // namespace hs::net
