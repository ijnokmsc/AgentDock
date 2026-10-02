// LogSink.h —— 启动器日志: 内存环形缓冲 + studio/logs/launcher.log 落盘
// 对齐 Qt6 版 MainWindow::log 行为 (时间戳行, 双写); 供 HTML 日志页 log.query 使用
#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace hs {

enum class LogLevel { Info, Warn, Error };

struct LogEntry {
    std::wstring ts;        // "HH:MM:SS"
    LogLevel    level;
    std::wstring text;
};

class LogSink {
public:
    void Init(const std::wstring& file);    // 文件为空 = 只记内存 (待构建模式下 studio 未定)
    void Log(LogLevel lvl, const std::wstring& text);

    // log.query: 倒序取最近 limit 条; level 空或 "all" = 全部; keyword 子串过滤
    std::vector<LogEntry> Query(const std::string& level, const std::string& keyword, int limit);

    static const wchar_t* LevelName(LogLevel l);
    static LogLevel LevelOfName(const std::string& s);

private:
    std::vector<LogEntry> ring_;            // 正序, 容量上限
    size_t head_ = 0;                       // 环形起点
    size_t total_ = 0;
    std::wstring file_;
    CRITICAL_SECTION cs_{};
};

LogSink& logSink();

}  // namespace hs
