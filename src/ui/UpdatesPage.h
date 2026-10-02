#pragma once

#include <QWidget>
#include <QTableWidget>
#include <QPushButton>
#include <QComboBox>
#include <QLabel>

#include "../core/Types.h"
#include "../core/Updater.h"

// 更新中心: 组件表格 + 检查更新 + 应用更新 + 通道选择
class UpdatesPage : public QWidget {
    Q_OBJECT
public:
    explicit UpdatesPage(QWidget* parent = nullptr);

    void setComponents(const QVector<hs::ComponentState>& states);
    void setBusy(bool busy);
    void setUpdating(bool updating, const QString& stage = {});
    void appendLog(const QString& line);

signals:
    void checkRequested(bool remote);
    void updateRequested(hs::ComponentId id);   // 对选中组件应用更新
    void updateVersionRequested(hs::ComponentId id);  // 需要用户选版本 (Node)

private slots:
    void onSelectionChanged();

private:
    void buildUi();

    QTableWidget*  table_     = nullptr;
    QComboBox*     channel_   = nullptr;
    QPushButton*   checkBtn_  = nullptr;
    QPushButton*   remoteBtn_ = nullptr;
    QPushButton*   updateBtn_ = nullptr;
    QLabel*        hint_      = nullptr;
    QLabel*        progress_  = nullptr;
    QVector<hs::ComponentState> states_;
};
