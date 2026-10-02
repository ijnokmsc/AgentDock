// LogSink.cpp
#include "LogSink.h"
#include "Ui.h"

#include <fstream>
#include <ctime>
#include <algorithm>

namespace hs {

namespace {
constexpr size_t kRingCap = 2000;

std::wstring NowTs() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[16];
    swprintf_s(buf, L"%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
    return buf;
}
}  // namespace

void LogSink::Init(const std::wstring& file) {
    file_ = file;
    InitializeCriticalSection(&cs_);
}

void LogSink::Log(LogLevel lvl, const std::wstring& text) {
    EnterCriticalSection(&cs_);
    LogEntry e{ NowTs(), lvl, text };
    if (ring_.size() < kRingCap) ring_.push_back(e);
    else { ring_[head_] = e; head_ = (head_ + 1) % kRingCap; }
    ++total_;
    if (!file_.empty()) {
        std::ofstream f(file_.c_str(), std::ios::app | std::ios::binary);
        if (f) {
            std::string line = "[" + WideToUtf8(e.ts) + "] [" + WideToUtf8(LevelName(lvl)) + "] "
                             + WideToUtf8(e.text) + "\n";
            f << line;
        }
    }
    LeaveCriticalSection(&cs_);
}

std::vector<LogEntry> LogSink::Query(const std::string& level, const std::string& keyword, int limit) {
    EnterCriticalSection(&cs_);
    std::vector<LogEntry> all;
    if (!ring_.empty()) {
        if (ring_.size() < kRingCap) all = ring_;
        else {
            all.reserve(kRingCap);
            for (size_t i = 0; i < kRingCap; ++i) all.push_back(ring_[(head_ + i) % kRingCap]);
        }
    }
    LeaveCriticalSection(&cs_);

    LogLevel want = LevelOfName(level);
    bool filterLevel = (level != "all" && !level.empty());
    std::vector<LogEntry> out;
    std::wstring kw = Utf8ToWide(keyword);
    for (auto it = all.rbegin(); it != all.rend() && (int)out.size() < limit; ++it) {
        if (filterLevel && it->level != want) continue;
        if (!kw.empty() && it->text.find(kw) == std::wstring::npos) continue;
        out.push_back(*it);
    }
    return out;
}

const wchar_t* LogSink::LevelName(LogLevel l) {
    switch (l) {
    case LogLevel::Warn:  return L"WARN";
    case LogLevel::Error: return L"ERROR";
    default:              return L"INFO";
    }
}

LogLevel LogSink::LevelOfName(const std::string& s) {
    if (s == "warn") return LogLevel::Warn;
    if (s == "error") return LogLevel::Error;
    return LogLevel::Info;
}

LogSink& logSink() {
    static LogSink g;
    return g;
}

}  // namespace hs
