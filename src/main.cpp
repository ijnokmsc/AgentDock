// AgentDock —— 多 Agent 便携启动器入口 (Qt 遗留版)
//
// 职责: 定位便携包根目录 -> 初始化应用 -> 打开主窗口
// 根目录判定: --root 参数 > settings.json > exe 同目录 > 向上查找

#include <QApplication>
#include <QMessageBox>
#include <QCommandLineParser>
#include <QFont>

#include "app/Paths.h"
#include "ui/MainWindow.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("AgentDock");
    app.setApplicationVersion(HERMESSTUDIO_VERSION);
    app.setOrganizationName("AgentDock");

    // 中文界面下避免 Qt 选到不便的缺省字体
    QFont f("Microsoft YaHei UI", 9);
    app.setFont(f);

    QCommandLineParser parser;
    parser.setApplicationDescription("AgentDock 多 Agent 便携启动器");
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption rootOpt("root", "指定 Hermes 便携包根目录", "dir");
    parser.addOption(rootOpt);
    parser.process(app);

    // ---- 定位便携包根目录 ----
    QString rootArg = parser.value(rootOpt);
    bool located = false;

    if (!rootArg.isEmpty()) {
        located = hs::Paths::setRoot(fs::path(rootArg.toStdString()));
        if (!located) {
            QMessageBox::critical(nullptr, "AgentDock",
                QString("指定的根目录不是合法的便携包:\n%1\n\n"
                        "需要同时包含 data\\、hermes-agent\\ 以及 venv\\ 或 python\\。")
                    .arg(rootArg));
            return 2;
        }
    } else {
        located = hs::Paths::autoDetect();
    }

    if (!located) {
        QMessageBox::critical(nullptr, "AgentDock",
            QString("未能定位便携包根目录。\n\n"
                    "请把 AgentDock.exe 放在便携包根目录 (与 data\\、hermes-agent\\ 同级), "
                    "或用 --root <目录> 显式指定。\n\n"
                    "当前 exe 位置: %1")
                .arg(QCoreApplication::applicationDirPath()));
        return 2;
    }

    MainWindow w;
    w.show();
    return app.exec();
}
