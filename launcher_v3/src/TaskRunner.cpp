// TaskRunner.cpp —— 后台任务 + UI marshal
#include "TaskRunner.h"

#include <thread>
#include "LogSink.h"
#include "Ui.h"

namespace hs {

namespace {
constexpr wchar_t kClass[] = L"HermesV3TaskRunner";
constexpr UINT WM_TASK_DONE = WM_APP + 81;

struct Job {
    std::function<void()> completion;   // UI 线程执行
};
}  // namespace

LRESULT CALLBACK TaskRunner::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_TASK_DONE) {
        auto* job = std::unique_ptr<Job>(reinterpret_cast<Job*>(w)).release();
        if (job) {
            auto done = std::move(job->completion);
            delete job;
            if (done) done();
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void TaskRunner::Init() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    // HWND_MESSAGE: message-only 窗口, 不参与屏幕/广播
    hwnd_ = CreateWindowExW(0, kClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                            wc.hInstance, nullptr);
}

void TaskRunner::Post(std::function<void()> job, std::function<void()> completion) {
    auto* j = new Job{ std::move(completion) };
    // detach 线程: 任务自包含 (completion 之外不触碰 UI); 结束后 PostMessage 回 UI
    std::thread([j, job = std::move(job), hwnd = hwnd_]() {
        try {
            if (job) job();
        } catch (const std::exception& e) {
            // 后台线程未捕获异常会 std::terminate 整个进程 (闪退), 必须兜底
            logSink().Log(LogLevel::Error,
                          Utf8ToWide(std::string("[task] \xe5\x90\x8e\xe5\x8f\xb0\xe4\xbb\xbb\xe5\x8a\xa1\xe5\xbc\x82\xe5\xb8\xb8: ") + e.what()));
        } catch (...) {
            logSink().Log(LogLevel::Error,
                          Utf8ToWide("[task] \xe5\x90\x8e\xe5\x8f\xb0\xe4\xbb\xbb\xe5\x8a\xa1\xe6\x9c\xaa\xe7\x9f\xa5\xe5\xbc\x82\xe5\xb8\xb8"));
        }
        PostMessageW(hwnd, WM_TASK_DONE, reinterpret_cast<WPARAM>(j), 0);
    }).detach();
}

TaskRunner& TaskRunner::Instance() {
    static TaskRunner g;
    return g;
}

}  // namespace hs
