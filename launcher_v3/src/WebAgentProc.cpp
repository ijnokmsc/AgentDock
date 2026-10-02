// WebAgentProc.cpp —— Agent 进程壳通用基类实现 (唯一一份相位机)
#include <winsock2.h>
#include <ws2tcpip.h>
#include "WebAgentProc.h"
#include "Ui.h"

#include <ws2def.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <psapi.h>
#include <shellapi.h>
#include <cstdio>

#pragma comment(lib, "psapi.lib")

namespace hs {

WebAgentProc::Status WebAgentProc::Query() {
    Status st;
    st.pid = ListenerPid();
    if (st.pid) {
        lastPhase_ = Phase::Running;
        st.phase = Phase::Running;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, st.pid);
        if (h) {
            PROCESS_MEMORY_COUNTERS pmc{};
            pmc.cb = sizeof(pmc);
            if (GetProcessMemoryInfo(h, &pmc, sizeof(pmc)))
                st.memMB = pmc.WorkingSetSize / (1024 * 1024);
            FILETIME ct{}, ce{}, ck{}, cu{};
            if (GetProcessTimes(h, &ct, &ce, &ck, &cu)) {
                ULARGE_INTEGER create{}, now{};
                create.LowPart = ct.dwLowDateTime;
                create.HighPart = ct.dwHighDateTime;
                GetSystemTimeAsFileTime(reinterpret_cast<FILETIME*>(&now));
                if (now.QuadPart > create.QuadPart)
                    st.uptimeSec = (now.QuadPart - create.QuadPart) / 10000000ULL;
            }
            CloseHandle(h);
        }
        return st;
    }

    // 端口未监听: 优先以 starting_ 锚点判定启动窗口
    if (starting_) {
        ULONGLONG elapsed = (GetTickCount64() - startAtMs_) / 1000;
        if (elapsed < kStartTimeoutSec) {
            lastPhase_ = Phase::Starting;
            st.phase = Phase::Starting;
            st.startingSec = elapsed;
            return st;
        }
        starting_ = false;             // 45s 超时
        lastPhase_ = Phase::Failed;
        st.phase = Phase::Failed;
        st.startingSec = elapsed;
        return st;
    }
    if (auto s = coreState()) {        // 仅 Hermes (Launcher 状态兜底)
        if (*s == HermesState::Starting) {
            ULONGLONG elapsed = (GetTickCount64() - startAtMs_) / 1000;
            if (elapsed < kStartTimeoutSec) {
                lastPhase_ = Phase::Starting;
                st.phase = Phase::Starting;
                st.startingSec = elapsed;
                return st;
            }
            lastPhase_ = Phase::Failed;
            st.phase = Phase::Failed;
            st.startingSec = elapsed;
            return st;
        }
        if (*s == HermesState::Stopping) {
            st.phase = Phase::Starting;    // 停止中按"过渡态"展示 (瞬时)
            return st;
        }
    }
    st.phase = (lastPhase_ == Phase::Running || lastPhase_ == Phase::Starting)
                 ? Phase::Failed      // 曾经活着, 端口消失 -> 掉线
                 : Phase::Stopped;
    return st;
}

void WebAgentProc::OpenBrowser() {
    ShellExecuteW(nullptr, L"open", pageUrl().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::wstring WebAgentProc::pageUrl() const {
    wchar_t b[64];
    swprintf_s(b, L"http://127.0.0.1:%d/", port_);
    return b;
}

void WebAgentProc::killByPid(DWORD pid) {
    wchar_t cmd[128];
    swprintf_s(cmd, L"taskkill /F /T /PID %lu", pid);
    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring c = cmd;
    if (CreateProcessW(nullptr, &c[0], nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        WaitForSingleObject(pi.hProcess, 8000);
        CloseHandle(pi.hProcess);
    }
}

std::wstring FormatUptime(ULONGLONG sec) {
    ULONGLONG h = sec / 3600, m = (sec % 3600) / 60, s = sec % 60;
    wchar_t buf[64];
    if (h > 0) swprintf_s(buf, L"%lluh %llum", (unsigned long long)h, (unsigned long long)m);
    else if (m > 0) swprintf_s(buf, L"%llum %llus", (unsigned long long)m, (unsigned long long)s);
    else swprintf_s(buf, L"%llus", (unsigned long long)s);
    return buf;
}

std::wstring FormatMem(ULONGLONG mb) {
    wchar_t buf[48];
    if (mb >= 1024) swprintf_s(buf, L"%.1f GB", mb / 1024.0);
    else swprintf_s(buf, L"%llu MB", (unsigned long long)mb);
    return buf;
}

const wchar_t* PhaseText(Phase p) {
    switch (p) {
    case Phase::Running:  return L"运行中";
    case Phase::Starting: return L"正在启动中";
    case Phase::Failed:   return L"已掉线";
    default:              return L"未运行";
    }
}

bool PhaseIsWarn(Phase p) { return p == Phase::Failed; }
bool PhaseIsBusy(Phase p) { return p == Phase::Starting; }

Phase PhaseFromHermes(HermesState s) {
    switch (s) {
    case HermesState::Running:  return Phase::Running;
    case HermesState::Starting: return Phase::Starting;
    case HermesState::Stopping: return Phase::Starting;
    case HermesState::Failed:   return Phase::Failed;
    default:                    return Phase::Stopped;
    }
}

}  // namespace hs
