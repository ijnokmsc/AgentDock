#pragma once

#include <QWidget>
#include <QTableWidget>
#include <QPushButton>
#include <QLabel>

#include "../core/PluginHost.h"

// 插件管理页: 自动扫描插件目录, 查看版本/作者/描述, 启用/停用/删除。
class PluginsPage : public QWidget {
    Q_OBJECT
public:
    explicit PluginsPage(QWidget* parent = nullptr);

    void setHost(hs::PluginHost* host);
    void reload();

signals:
    void statusMessage(const QString& text);

private slots:
    void onEnableDisable();
    void onRemove();
    void onRefresh();

private:
    void buildUi();
    void refresh();

    hs::PluginHost* host_ = nullptr;
    QTableWidget* table_  = nullptr;
    QPushButton*  toggleBtn_ = nullptr;
    QPushButton*  removeBtn_ = nullptr;
    QPushButton*  refreshBtn_ = nullptr;
    QLabel*       hint_   = nullptr;
};
