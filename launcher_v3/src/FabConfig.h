// FabConfig.h —— 悬浮按钮配置 (data/config/fab.json)
//
// 方案2 第九节: 位置与配置存移动硬盘, 不写注册表 / %APPDATA%。
//   { "enabled": true, "doubleClickTimeMs": 500, "singleClickMode": "deferred",
//     "position": {"x":1240,"y":760}, "hideWhenMainVisible": false }
#pragma once
#include <string>

namespace hs {

struct FabConfig {
    bool enabled            = true;
    int  doubleClickTimeMs  = 0;    // 0 = 跟随系统 GetDoubleClickTime()
    bool immediateClick     = false;// true = 单击立即弹菜单 (双击退化为两次单击)
    int  x                  = -1;   // 屏幕坐标; -1 = 默认主屏右下角
    int  y                  = -1;
    bool hideWhenMainVisible= false;// 主面板可见时隐藏悬浮按钮
    bool stopHermesOnExit   = false;// 退出启动器时一并停止 Hermes

    // 主窗口位置大小 (-1 = 未记录, 用系统默认); wndMax = 上次关闭时最大化
    int  wndX = -1, wndY = -1, wndW = -1, wndH = -1;
    bool wndMax = false;

    // 点 X 关闭时的行为: 0=每次询问 1=最小化到托盘 2=直接退出 (记住选择)
    int  closeAction = 0;

    // 读写 <root>/data/config/fab.json; 文件缺失/损坏时用默认值
    void Load(const std::wstring& path);
    void Save(const std::wstring& path) const;

    int ClickWindowMs() const;      // 实际生效的单击/双击判定窗口
};

}  // namespace hs
