// TaskRunner.h —— 后台任务执行 + UI 线程回传
// Win32 版的 QtConcurrent/Fl::awake 等价: 任务在专用 std::thread 跑,
// 完成回调 marshal 回 UI 线程 (自建 message-only 窗口, 不纠缠主窗消息)。
#pragma once
#include <windows.h>
#include <functional>

namespace hs {

class TaskRunner {
public:
    void Init();                            // 创建 message-only 窗口 (UI 线程调用)
    // job 在后台线程执行; completion 在 UI 线程执行 (可为空)
    void Post(std::function<void()> job, std::function<void()> completion = nullptr);

    static TaskRunner& Instance();

private:
    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l);
    HWND hwnd_ = nullptr;
};

}  // namespace hs
