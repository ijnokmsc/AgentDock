#include "PluginsPage.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>

PluginsPage::PluginsPage(QWidget* parent) : QWidget(parent) { buildUi(); }

void PluginsPage::buildUi() {
    auto* lay = new QVBoxLayout(this);

    hint_ = new QLabel(
        "插件通过 plugin/<id>/plugin.dll 提供扩展能力。"
        "自动扫描插件目录, 默认全部启用。此处可查看版本/作者, 启用/停用/删除插件。", this);
    hint_->setWordWrap(true);
    hint_->setStyleSheet("color: #555; padding: 4px;");
    lay->addWidget(hint_);

    table_ = new QTableWidget(this);
    table_->setColumnCount(6);
    table_->setHorizontalHeaderLabels({"插件 ID", "名称", "版本", "作者", "状态", "说明"});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    table_->verticalHeader()->setVisible(false);
    lay->addWidget(table_);

    auto* btn = new QHBoxLayout();
    toggleBtn_  = new QPushButton("启用 / 停用", this);
    removeBtn_  = new QPushButton("删除插件", this);
    refreshBtn_ = new QPushButton("重新扫描", this);
    btn->addWidget(toggleBtn_);
    btn->addWidget(removeBtn_);
    btn->addWidget(refreshBtn_);
    btn->addStretch();
    lay->addLayout(btn);

    connect(toggleBtn_, &QPushButton::clicked, this, &PluginsPage::onEnableDisable);
    connect(removeBtn_, &QPushButton::clicked, this, &PluginsPage::onRemove);
    connect(refreshBtn_, &QPushButton::clicked, this, &PluginsPage::onRefresh);
}

void PluginsPage::setHost(hs::PluginHost* host) { host_ = host; }

void PluginsPage::reload() { refresh(); }

void PluginsPage::refresh() {
    if (!host_) return;
    auto plugins = host_->allPlugins();
    table_->setRowCount(0);
    for (const auto& p : plugins) {
        int row = table_->rowCount();
        table_->insertRow(row);
        table_->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(p.id)));
        table_->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(p.name)));
        table_->setItem(row, 2, new QTableWidgetItem(QString::fromStdString(p.version)));
        table_->setItem(row, 3, new QTableWidgetItem(QString::fromStdString(p.author)));
        auto* st = new QTableWidgetItem(p.enabled ? "已启用" : "已停用");
        st->setForeground(p.enabled ? QColor("#137333") : QColor("#999"));
        table_->setItem(row, 4, st);
        table_->setItem(row, 5, new QTableWidgetItem(QString::fromStdString(p.description)));
    }
    bool hasSelection = false;
    if (table_->rowCount() > 0) { table_->selectRow(0); hasSelection = true; }
    toggleBtn_->setEnabled(hasSelection);
    removeBtn_->setEnabled(hasSelection);
}

void PluginsPage::onEnableDisable() {
    int row = table_->currentRow();
    if (row < 0 || !host_) return;
    auto plugins = host_->allPlugins();
    if (row >= (int)plugins.size()) return;
    const auto& p = plugins[row];

    std::string err;
    if (!host_->setEnabled(p.id, !p.enabled, &err)) {
        emit statusMessage(QString::fromStdString(err));
        return;
    }
    host_->reload();
    refresh();
    emit statusMessage(QString("插件 %1 已%2").arg(QString::fromStdString(p.name),
                          p.enabled ? "停用" : "启用"));
}

void PluginsPage::onRemove() {
    int row = table_->currentRow();
    if (row < 0 || !host_) return;
    auto plugins = host_->allPlugins();
    if (row >= (int)plugins.size()) return;
    const auto& p = plugins[row];

    if (QMessageBox::question(this, "确认删除",
            QString("删除插件 \"%1\" (%2)? 将移除其插件目录。").arg(
                QString::fromStdString(p.name), QString::fromStdString(p.version)))
        != QMessageBox::Yes) return;

    std::string err;
    if (!host_->removePlugin(p.id, &err)) {
        emit statusMessage(QString::fromStdString(err));
        return;
    }
    refresh();
    emit statusMessage(QString("插件已删除: %1").arg(QString::fromStdString(p.name)));
}

void PluginsPage::onRefresh() {
    if (host_) host_->reload();
    refresh();
    emit statusMessage("已重新扫描插件目录");
}
