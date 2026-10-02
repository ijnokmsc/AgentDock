#pragma once

#include <QWidget>
#include <QTreeWidget>
#include <QPushButton>
#include <QLabel>

#include "../core/Types.h"

// 体检页: 检查项列表 (图标 + 名称 + 详情 + 修复按钮)
class DiagnosticsPage : public QWidget {
    Q_OBJECT
public:
    explicit DiagnosticsPage(QWidget* parent = nullptr);

    void setItems(const QVector<hs::CheckItem>& items);
    void setSummary(const QString& text);

signals:
    void recheckRequested();
    void fixRequested(const QString& action);

private:
    void buildUi();

    QTreeWidget* tree_    = nullptr;
    QLabel*      summary_ = nullptr;
    QPushButton* recheckBtn_ = nullptr;
    QPushButton* fixAllBtn_  = nullptr;
};
