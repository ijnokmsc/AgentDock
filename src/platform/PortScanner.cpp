#include "PortScanner.h"

// winsock2 必须在 windows.h 之前包含, 否则 AF_INET / ntohs 等符号不可见
#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>
#include <iphlpapi.h>
#include <tcpmib.h>
#include <tlhelp32.h>

#include <string>
#include <memory>

#pragma comment(lib, "iphlpapi.lib")

namespace hs::net {

namespace {

// 单协议栈监听表扫描 (V4/V6 表结构不同但布局同构, 用模板抹平)
template <typename TableT, DWORD family>
std::optional<unsigned long> scanStack(int port) {
    DWORD size = 0;
    if (GetExtendedTcpTable(nullptr, &size, FALSE, family, TCP_TABLE_OWNER_PID_LISTENER, 0)
        != ERROR_INSUFFICIENT_BUFFER || size == 0)
        return std::nullopt;
    std::unique_ptr<BYTE[]> buf(new BYTE[size]);
    auto* table = reinterpret_cast<TableT*>(buf.get());
    if (GetExtendedTcpTable(buf.get(), &size, FALSE, family, TCP_TABLE_OWNER_PID_LISTENER, 0)
        != NO_ERROR)
        return std::nullopt;
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& row = table->table[i];
        if (ntohs((u_short)row.dwLocalPort) == (u_short)port)
            return (unsigned long)row.dwOwningPid;
    }
    return std::nullopt;
}

} // namespace

// 双协议栈: 网关可能监听 V4 或 V6, 只查一个协议栈会永远探测不到
// (曾致启动器在 IPv6 监听场景永远显示"启动中"; 两个 Agent 进程壳共用)
std::optional<unsigned long> pidByPort(int port) {
    if (auto pid = scanStack<MIB_TCPTABLE_OWNER_PID, AF_INET>(port)) return pid;
    return scanStack<MIB_TCP6TABLE_OWNER_PID, AF_INET6>(port);
}

bool isPortInUse(int port) {
    return pidByPort(port).has_value();
}

std::string processName(unsigned long pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return {};
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    std::string name;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == (DWORD)pid) {
                int n = WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, nullptr, 0, nullptr, nullptr);
                std::string tmp((size_t)(n > 0 ? n - 1 : 0), '\0');
                WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, tmp.data(), n, nullptr, nullptr);
                name = tmp;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return name;
}

} // namespace hs::net
