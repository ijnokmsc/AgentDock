#pragma once

#include <QWidget>
#include <QComboBox>
#include <QLabel>
#include <QTextEdit>
#include <QPushButton>

#include "../core/Types.h"

// 概览页: 状态卡片 + 启动模式 + 启动/停止 + 最近日志
class HomePage : public QWidget {
    Q_OBJECT
public:
    explicit HomePage(QWidget* parent = nullptr);

    void setRootPath(const QString& v);
    void setKernelVersion(const QString& v);
    void setWebUiVersion(const QString& v);
    void setDefaultModel(const QString& v);
    void setState(hs::HermesState state);
    void setLaunchBlocked(bool blocked, const QString& reason);
    void appendLog(const QString& line);

    hs::StartMode selectedMode() const;
    void setModeAvailable(hs::StartMode mode, bool available, const QString& reason);

signals:
    void startRequested(hs::StartMode mode);
    void stopRequested();
    void openBrowserRequested();

private:
    void buildUi();

    QLabel*    rootValue_     = nullptr;
    QLabel*    kernelValue_   = nullptr;
    QLabel*    webUiValue_    = nullptr;
    QLabel*    modelValue_    = nullptr;
    QLabel*    stateValue_    = nullptr;
    QComboBox* modeCombo_     = nullptr;
    QPushButton* startBtn_    = nullptr;
    QPushButton* stopBtn_     = nullptr;
    QPushButton* browserBtn_  = nullptr;
    QLabel*    blockHint_     = nullptr;
    QTextEdit* logView_       = nullptr;
};
