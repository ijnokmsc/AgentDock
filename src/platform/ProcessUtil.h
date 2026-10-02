#pragma once

#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <windows.h>

namespace fs = std::filesystem;

namespace hs::proc {

struct LaunchOptions {
    fs::path                exe;
    std::vector<std::string> args;
    fs::path                workingDir;
    bool                    newConsole   = false;   // CLI 需要独立控制台窗口
    bool                    hidden       = false;   // WebUI 后台运行
    std::vector<std::pair<std::string, std::string>> env;  // 追加/覆盖的环境变量
    // 可选: 把子进程 stdout+stderr 接到匿名管道读柄上 (调用方 CreatePipe 出
    // 一对, 把写柄传进来; 调用方负责在读完后 CloseHandle 读柄, 写柄由子进程
    // 继承、launch 内部会先关闭父端副本避免 EOF 永不到来)。
    // 设置后子进程以 bInheritHandles=TRUE 启动。
    HANDLE                  hStdoutPipe = nullptr;
};

// 启动进程, 返回 PID
std::optional<unsigned long> launch(const LaunchOptions& opt, std::string* err = nullptr);

// 进程是否仍在运行
bool isRunning(unsigned long pid);

// 终止进程及其子进程树
bool killTree(unsigned long pid, int waitMs = 5000);

// 友好关闭: 发 CTRL_BREAK / WM_CLOSE, 失败再 killTree
bool gracefulStop(unsigned long pid, int waitMs = 5000);

// 同步执行并等待结束, 返回退出码 (超时返回 -1)
int runAndWait(const LaunchOptions& opt, int timeoutMs = 60000, std::string* err = nullptr);

// 同步执行并捕获 stdout+stderr。
// 走匿名管道而不是 "cmd /c ... > file" —— 后者会被 buildCommandLine 的引号转义
// 破坏 (内层引号变字面量, 重定向失效), 曾导致 node/python/uv 版本探测全部落空。
struct CaptureResult {
    int         exitCode = -1;
    std::string output;          // stdout + stderr 合并
    bool        timedOut = false;
};
CaptureResult runAndCapture(const LaunchOptions& opt, int timeoutMs = 15000, std::string* err = nullptr);

// 从 JSON 文件 (如 data/gateway.pid) 读出 pid
std::optional<unsigned long> pidFromFile(const fs::path& file);

} // namespace hs::proc
