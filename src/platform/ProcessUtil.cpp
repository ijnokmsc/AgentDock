#include "ProcessUtil.h"

#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>

#include <fstream>
#include <cctype>

#include <nlohmann/json.hpp>

namespace {

std::wstring toWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

// 单个参数 -> Windows 风格带引号转义
std::wstring quoteArg(const std::string& s) {
    std::string escaped;
    escaped.reserve(s.size() + 8);
    size_t backslashes = 0;
    for (char c : s) {
        if (c == '\\') { ++backslashes; escaped += c; }
        else if (c == '"') { escaped.append(backslashes * 2, '\\'); backslashes = 0; escaped += "\\\""; }
        else { backslashes = 0; escaped += c; }
    }
    escaped.append(backslashes * 2, '\\');
    return L"\"" + toWide(escaped) + L"\"";
}

// 组装命令行: 每个参数用双引号包裹, 并对引号/反斜杠做 Windows 风格转义
std::wstring buildCommandLine(const std::filesystem::path& exe,
                              const std::vector<std::string>& args) {
    std::wstring cmd = quoteArg(exe.string());
    for (const auto& a : args) {
        cmd += L' ';
        cmd += quoteArg(a);
    }
    return cmd;
}

std::wstring comSpec() {
    wchar_t buf[MAX_PATH] = {0};
    DWORD n = GetEnvironmentVariableW(L"ComSpec", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"C:\\Windows\\System32\\cmd.exe";
    return std::wstring(buf, n);
}

// .cmd / .bat 不能直接当 lpApplicationName 交给 CreateProcess
// (会失败), 必须显式走 cmd.exe /s /c ""脚本" args"。
// 官方 runtime 的 hermes 入口就是 `venv\Scripts\hermes.cmd`, 所以这条路必须通。
bool isBatchFile(const std::filesystem::path& p) {
    std::string ext = p.extension().string();
    for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
    return ext == ".cmd" || ext == ".bat";
}

// 返回 {lpApplicationName, lpCommandLine}
struct LaunchSpec {
    std::wstring app;
    std::wstring cmd;
};

LaunchSpec buildLaunchSpec(const std::filesystem::path& exe,
                           const std::vector<std::string>& args) {
    LaunchSpec s;
    if (isBatchFile(exe)) {
        s.app = comSpec();
        std::wstring inner = quoteArg(exe.string());
        for (const auto& a : args) { inner += L' '; inner += quoteArg(a); }
        // /s + 外层再加一层引号 = cmd 引号解析的确定形式
        s.cmd = L"\"" + s.app + L"\" /s /c \"" + inner + L"\"";
    } else {
        s.app = exe.wstring();
        s.cmd = buildCommandLine(exe, args);
    }
    return s;
}

std::vector<unsigned long> childPids(unsigned long parent) {
    std::vector<unsigned long> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ParentProcessID == parent) out.push_back(pe.th32ProcessID);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

// 环境块: 父进程环境 + opt.env 追加/覆盖 (launch 与 runAndCapture 共用)。
// 曾只在 launch 实现 —— runAndCapture 传 nullptr 导致插件装/卸落到宿主
// ~/.dsh 而非便携包 DSH_HOME, 便携化契约被破坏。
std::wstring buildEnvBlock(const std::vector<std::pair<std::string, std::string>>& env) {
    std::wstring block;
    if (env.empty()) return block;
    std::vector<std::pair<std::wstring, std::wstring>> merged;
    if (LPWCH raw = GetEnvironmentStringsW()) {
        LPWCH p = raw;
        while (*p) {
            std::wstring entry = p;
            p += entry.size() + 1;
            auto eq = entry.find(L'=');
            if (eq != std::wstring::npos && eq > 0)
                merged.emplace_back(entry.substr(0, eq), entry.substr(eq + 1));
        }
        FreeEnvironmentStringsW(raw);
    }
    for (const auto& kv : env) {
        std::wstring k = toWide(kv.first), v = toWide(kv.second);
        bool found = false;
        for (auto& m : merged) {
            if (_wcsicmp(m.first.c_str(), k.c_str()) == 0) { m.second = v; found = true; break; }
        }
        if (!found) merged.emplace_back(k, v);
    }
    for (const auto& m : merged) {
        block += m.first; block += L'='; block += m.second; block += L'\0';
    }
    block += L'\0';
    return block;
}
} // namespace

namespace hs::proc {

std::optional<unsigned long> launch(const LaunchOptions& opt, std::string* err) {
    if (opt.exe.empty()) {
        if (err) *err = "可执行文件未指定";
        return std::nullopt;
    }
    std::error_code ec;
    if (!std::filesystem::exists(opt.exe, ec)) {
        if (err) *err = "可执行文件不存在: " + opt.exe.string();
        return std::nullopt;
    }

    LaunchSpec spec = buildLaunchSpec(opt.exe, opt.args);
    std::wstring cmd = spec.cmd;

    // 环境块: 先取当前进程环境, 再追加/覆盖
    std::wstring envBlock = buildEnvBlock(opt.env);

    DWORD flags = CREATE_UNICODE_ENVIRONMENT;
    if (opt.newConsole) flags |= CREATE_NEW_CONSOLE;
    if (opt.hidden)      flags |= CREATE_NO_WINDOW;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = opt.newConsole ? SW_SHOW : SW_HIDE;

    // stdout 管道 (可选): 写柄给子进程继承, 父端副本立即关闭 (否则读端永不 EOF)
    HANDLE inheritWrite = nullptr;
    BOOL inherit = FALSE;
    if (opt.hStdoutPipe) {
        inherit = TRUE;
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdInput  = nullptr;
        si.hStdOutput = opt.hStdoutPipe;
        si.hStdError  = opt.hStdoutPipe;
        DuplicateHandle(GetCurrentProcess(), opt.hStdoutPipe, GetCurrentProcess(),
                        &inheritWrite, 0, TRUE, DUPLICATE_SAME_ACCESS);
        si.hStdOutput = si.hStdError = inheritWrite;
    }

    PROCESS_INFORMATION pi{};
    std::wstring wd = opt.workingDir.empty() ? std::wstring() : opt.workingDir.wstring();

    BOOL ok = CreateProcessW(
        spec.app.c_str(),
        cmd.data(),
        nullptr, nullptr, inherit,
        flags,
        envBlock.empty() ? nullptr : envBlock.data(),
        wd.empty() ? nullptr : wd.c_str(),
        &si, &pi);

    if (inheritWrite) CloseHandle(inheritWrite);

    if (!ok) {
        if (err) *err = "CreateProcess 失败, GetLastError=" + std::to_string(GetLastError());
        return std::nullopt;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (unsigned long)pi.dwProcessId;
}

bool isRunning(unsigned long pid) {
    if (pid == 0) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) return false;
    DWORD code = 0;
    BOOL ok = GetExitCodeProcess(h, &code);
    CloseHandle(h);
    return ok && code == STILL_ACTIVE;
}

bool killTree(unsigned long pid, int waitMs) {
    if (pid == 0) return true;

    // 先杀子进程 (node 会 spawn 子进程, hermes gateway 同理)
    for (unsigned long child : childPids(pid)) {
        killTree(child, waitMs / 2 > 0 ? waitMs / 2 : 500);
    }

    HANDLE h = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) return true;  // 已经不在了
    BOOL ok = TerminateProcess(h, 1);
    CloseHandle(h);
    if (!ok) return false;

    // 等待真正退出
    DWORD waited = 0;
    while (waited < (DWORD)waitMs) {
        if (!isRunning(pid)) return true;
        Sleep(100);
        waited += 100;
    }
    return !isRunning(pid);
}

bool gracefulStop(unsigned long pid, int waitMs) {
    if (pid == 0) return true;

    // 1) 若目标有控制台, 发 CTRL_BREAK 让它自己走清理流程
    if (AttachConsole((DWORD)pid)) {
        SetConsoleCtrlHandler(nullptr, TRUE);
        GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, (DWORD)pid);
        FreeConsole();
        SetConsoleCtrlHandler(nullptr, FALSE);

        DWORD waited = 0;
        while (waited < (DWORD)waitMs) {
            if (!isRunning(pid)) return true;
            Sleep(100);
            waited += 100;
        }
    }

    // 2) 兜底强杀
    return killTree(pid, 2000);
}

int runAndWait(const LaunchOptions& opt, int timeoutMs, std::string* err) {
    if (opt.exe.empty()) {
        if (err) *err = "可执行文件未指定";
        return -1;
    }
    auto pid = launch(opt, err);
    if (!pid) return -1;

    // 轮询等待退出 (避免引入句柄等待的复杂度, 100ms 粒度足够)
    int waited = 0;
    while (waited < timeoutMs) {
        if (!isRunning(*pid)) break;
        Sleep(100);
        waited += 100;
    }
    if (isRunning(*pid)) {
        killTree(*pid, 2000);
        if (err) *err = "执行超时被终止";
        return -1;
    }
    return 0;
}

CaptureResult runAndCapture(const LaunchOptions& opt, int timeoutMs, std::string* err) {
    CaptureResult r;
    if (opt.exe.empty()) {
        if (err) *err = "可执行文件未指定";
        return r;
    }
    std::error_code ec;
    if (!fs::exists(opt.exe, ec)) {
        if (err) *err = "可执行文件不存在: " + opt.exe.string();
        return r;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;          // 子进程要继承写端

    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
        if (err) *err = "CreatePipe 失败, GetLastError=" + std::to_string(GetLastError());
        return r;
    }
    // 读端不可继承, 否则子进程退出后管道不会关闭 -> ReadFile 永远阻塞
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    LaunchSpec spec = buildLaunchSpec(opt.exe, opt.args);
    std::wstring cmd = spec.cmd;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags   = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWrite;
    si.hStdError  = hWrite;            // stderr 合并进同一管道

    PROCESS_INFORMATION pi{};
    std::wstring wd = opt.workingDir.empty() ? std::wstring() : opt.workingDir.wstring();
    std::wstring envBlock = buildEnvBlock(opt.env);   // 曾传 nullptr: opt.env 被忽略

    BOOL ok = CreateProcessW(
        spec.app.c_str(), cmd.data(),
        nullptr, nullptr,
        TRUE,                          // 继承句柄 (管道写端)
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
        envBlock.empty() ? nullptr : envBlock.data(),
        wd.empty() ? nullptr : wd.c_str(),
        &si, &pi);

    if (!ok) {
        if (err) *err = "CreateProcess 失败, GetLastError=" + std::to_string(GetLastError());
        CloseHandle(hRead); CloseHandle(hWrite);
        return r;
    }
    CloseHandle(pi.hThread);
    CloseHandle(hWrite);               // 父进程放弃写端, 子进程退出后 ReadFile 才会返回 0

    // 边等边读: 输出量超过管道缓冲(默认 4KB)时, 先 Wait 再 Read 会死锁
    std::string buf;
    const DWORD kChunk = 8192;
    DWORD waited = 0;
    const DWORD step = 50;
    bool reaped = false;
    while (waited < (DWORD)timeoutMs) {
        DWORD avail = 0;
        if (PeekNamedPipe(hRead, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            std::string chunk(avail, '\0');
            DWORD got = 0;
            if (ReadFile(hRead, chunk.data(), (DWORD)chunk.size(), &got, nullptr) && got > 0) {
                buf.append(chunk.data(), got);
                continue;              // 还有数据, 继续读而不是空转
            }
        }
        if (!reaped && WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) reaped = true;
        if (reaped && avail == 0) break;
        Sleep(step);
        waited += step;
    }
    // 收尾: 把剩余数据读完 (循环因超时退出时也要尽力拿)
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(hRead, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) break;
        std::string chunk(avail, '\0');
        DWORD got = 0;
        if (!ReadFile(hRead, chunk.data(), (DWORD)chunk.size(), &got, nullptr) || got == 0) break;
        buf.append(chunk.data(), got);
    }

    if (!reaped) {
        if (WaitForSingleObject(pi.hProcess, 0) != WAIT_OBJECT_0) {
            TerminateProcess(pi.hProcess, 1);
            r.timedOut = true;
        }
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    r.exitCode = (int)code;

    CloseHandle(hRead);
    CloseHandle(pi.hProcess);

    // utf-8 直通; 若含 GBK 中文也不会影响版本号提取
    r.output = buf;
    return r;
}

std::optional<unsigned long> pidFromFile(const fs::path& file) {
    std::error_code ec;
    if (!fs::exists(file, ec)) return std::nullopt;
    std::ifstream in(file);
    if (!in) return std::nullopt;
    try {
        auto j = nlohmann::json::parse(in);
        if (j.is_object() && j.contains("pid") && j["pid"].is_number_unsigned()) {
            return j["pid"].get<unsigned long>();
        }
    } catch (const std::exception&) {
        // 不是 JSON (例如 .hermes.lock 只是个纯数字) -> 退回纯数字解析
    }
    std::ifstream in2(file);
    unsigned long v = 0;
    if (in2 >> v && v != 0) return v;
    return std::nullopt;
}

} // namespace hs::proc
