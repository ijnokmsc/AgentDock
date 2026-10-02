#pragma once

#include <optional>
#include <string>

namespace hs::net {

// 端口是否处于 LISTENING 状态
bool isPortInUse(int port);

// 占用该端口的进程 PID
std::optional<unsigned long> pidByPort(int port);

// 获取进程名 (用于向用户展示 "谁占了端口")
std::string processName(unsigned long pid);

} // namespace hs::net
