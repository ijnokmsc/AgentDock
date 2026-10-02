// FabConfig.cpp —— fab.json 读写 (nlohmann/json)
#include "FabConfig.h"

#include <windows.h>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace hs {
namespace {
std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
}  // namespace

void FabConfig::Load(const std::wstring& path) {
    *this = FabConfig{};
    OutputDebugStringW((L"[fab] Load from: " + path).c_str());   // 先恢复默认, 再覆盖
    try {
        std::ifstream f(path.c_str(), std::ios::binary);
        if (!f) return;
        nlohmann::json j = nlohmann::json::parse(f, nullptr, true, /*allow_comments*/ true);

        if (j.contains("enabled"))             enabled = j["enabled"].get<bool>();
        if (j.contains("doubleClickTimeMs"))   doubleClickTimeMs = j["doubleClickTimeMs"].get<int>();
        if (j.contains("singleClickMode")) {
            auto m = j["singleClickMode"].get<std::string>();
            immediateClick = (m == "immediate");
        }
        if (j.contains("stopHermesOnExit"))    stopHermesOnExit = j["stopHermesOnExit"].get<bool>();
        if (j.contains("hideWhenMainVisible")) hideWhenMainVisible = j["hideWhenMainVisible"].get<bool>();
        if (j.contains("position")) {
            auto& p = j["position"];
            if (p.contains("x")) x = p["x"].get<int>();
            if (p.contains("y")) y = p["y"].get<int>();
        }
        if (j.contains("closeAction"))         closeAction = j["closeAction"].get<int>();
        if (j.contains("window")) {
            OutputDebugStringW(L"[fab] window key found");
            auto& w = j["window"];
            if (w.contains("x")) wndX = w["x"].get<int>();
            if (w.contains("y")) wndY = w["y"].get<int>();
            if (w.contains("w")) wndW = w["w"].get<int>();
            if (w.contains("h")) wndH = w["h"].get<int>();
            if (w.contains("maximized")) wndMax = w["maximized"].get<bool>();
        }
    } catch (...) {
        // 损坏的配置不阻断启动, 保持默认值
    }
}

void FabConfig::Save(const std::wstring& path) const {
    try {
        nlohmann::json j;
        j["enabled"]             = enabled;
        j["doubleClickTimeMs"]   = doubleClickTimeMs;
        j["singleClickMode"]     = immediateClick ? "immediate" : "deferred";
        j["stopHermesOnExit"]    = stopHermesOnExit;
        j["hideWhenMainVisible"] = hideWhenMainVisible;
        j["position"] = { {"x", x}, {"y", y} };
        j["window"] = { {"x", wndX}, {"y", wndY}, {"w", wndW}, {"h", wndH},
                        {"maximized", wndMax} };
        j["closeAction"] = closeAction;

        // 确保父目录存在 (data\config 整条链都可能缺; CreateDirectoryW 不递归)
        std::wstring dir = path.substr(0, path.find_last_of(L"\\/"));
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);

        std::ofstream f(path.c_str(), std::ios::binary | std::ios::trunc);
        f << j.dump(2);
    } catch (...) {
    }
}

int FabConfig::ClickWindowMs() const {
    if (doubleClickTimeMs > 0) return doubleClickTimeMs;
    return GetDoubleClickTime();
}

}  // namespace hs
