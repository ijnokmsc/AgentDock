#pragma once

#include "Types.h"
#include "../app/Paths.h"
#include <functional>

namespace hs {

// 启动前体检。每项检查产出 CheckItem{level, detail, fixAction}。
//
// fixAction 约定:
//   "auto:<id>"     启动器可自动修复
//   "goto:<page>"   需要用户去某个页面处理
//   ""              只能给提示
class PreflightChecker {
public:
    struct Context {
        ModelConfig   modelConfig;
        RuntimeConfig runtime;
        bool          haveModels = false;   // models.json 是否已存在
        bool          hermesRunning = false; // Hermes 是否正在运行 (运行中端口被占是正常)
    };

    explicit PreflightChecker(Context ctx);

    std::vector<CheckItem> runAll();

    // 执行自动修复; 返回修复后的说明
    std::string applyFix(const std::string& action);

    // 整体是否可启动
    bool canLaunch() const;

private:
    CheckItem checkRootLayout() const;
    CheckItem checkRuntime() const;
    CheckItem checkConfigFiles() const;
    CheckItem checkPorts() const;
    CheckItem checkEditablePaths() const;
    CheckItem checkStaleLocks() const;

    Context ctx_;
    std::vector<CheckItem> last_;
};

} // namespace hs
