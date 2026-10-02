#include "UpdatesPage.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QDateTime>
#include <QInputDialog>

UpdatesPage::UpdatesPage(QWidget* parent) : QWidget(parent) { buildUi(); }

void UpdatesPage::buildUi() {
    auto* lay = new QVBoxLayout(this);

    hint_ = new QLabel(
        "选择组件后点击「更新选中」即可下载 → 校验 → 备份 → 替换。"
        "内核与 Web UI 更新后会重启 Hermes 生效; 更新前会自动备份到 studio/backups/, 可回滚。", this);
    hint_->setWordWrap(true);
    hint_->setStyleSheet("color: #555; padding: 4px;");
    lay->addWidget(hint_);

    table_ = new QTableWidget(this);
    table_->setColumnCount(4);
    table_->setHorizontalHeaderLabels({"组件", "当前版本", "最新版本", "安装位置 / 备注"});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    table_->verticalHeader()->setVisible(false);
    lay->addWidget(table_);

    auto* btnLay = new QHBoxLayout();
    channel_ = new QComboBox(this);
    channel_->addItem("stable", "stable");
    channel_->addItem("beta", "beta");
    checkBtn_  = new QPushButton("重新探测本地", this);
    remoteBtn_ = new QPushButton("检查远端更新", this);
    updateBtn_ = new QPushButton("更新选中", this);
    updateBtn_->setEnabled(false);
    updateBtn_->setToolTip("下载选中有更新的组件并应用, 自动备份可回滚");

    btnLay->addWidget(new QLabel("通道:", this));
    btnLay->addWidget(channel_);
    btnLay->addWidget(checkBtn_);
    btnLay->addWidget(remoteBtn_);
    btnLay->addWidget(updateBtn_);
    btnLay->addStretch();
    lay->addLayout(btnLay);

    progress_ = new QLabel(this);
    progress_->setWordWrap(true);
    progress_->setStyleSheet("color: #333; padding: 4px;");
    progress_->hide();
    lay->addWidget(progress_);

    connect(checkBtn_, &QPushButton::clicked, this, [this] { emit checkRequested(false); });
    connect(remoteBtn_, &QPushButton::clicked, this, [this] { emit checkRequested(true); });
    connect(updateBtn_, &QPushButton::clicked, this, [this] {
        int row = table_->currentRow();
        if (row >= 0 && row < (int)states_.size() && states_[row].updateAvailable) {
            auto id = states_[row].id;
            if (id == hs::ComponentId::NodeRuntime)
                emit updateVersionRequested(id);   // Node: 让用户挑版本
            else
                emit updateRequested(id);
        }
    });
    connect(table_, &QTableWidget::itemSelectionChanged, this, &UpdatesPage::onSelectionChanged);
}

void UpdatesPage::setComponents(const QVector<hs::ComponentState>& states) {
    states_ = states;
    table_->setRowCount(0);
    for (const auto& s : states) {
        int row = table_->rowCount();
        table_->insertRow(row);

        auto* name = new QTableWidgetItem(QString::fromStdString(s.name));
        if (auto d = hs::Updater::defOf(s.id)) {
            if (d->highRisk) {
                name->setForeground(QColor("#b26a00"));
                name->setToolTip("高风险组件: 更新后可能导致 venv 失效, 需二次确认");
            }
        }
        table_->setItem(row, 0, name);

        QString cur = QString::fromStdString(s.currentVersion);
        table_->setItem(row, 1, new QTableWidgetItem(cur.isEmpty() ? "未探测到" : cur));

        QString lat = QString::fromStdString(s.latestVersion);
        QString latText = lat.isEmpty() ? "-" : lat;
        auto* l = new QTableWidgetItem(s.updateAvailable ? ("→ " + latText) : latText);
        if (s.updateAvailable) {
            l->setForeground(QColor("#c5221f"));
            QFont f = l->font(); f.setBold(true); l->setFont(f);
        }
        table_->setItem(row, 2, l);

        QString note = QString::fromStdString(s.installPath);
        if (s.updateAvailable) note += "  [有更新]";
        if (!s.supported) note = "不可用: " + QString::fromStdString(s.note);
        table_->setItem(row, 3, new QTableWidgetItem(note));
    }
    onSelectionChanged();
}

void UpdatesPage::onSelectionChanged() {
    int row = table_->currentRow();
    bool supported = false;
    if (row >= 0 && row < (int)states_.size()) {
        auto id = states_[row].id;
        // 支持一键更新: 内核/Web UI (官方源) + Node (用桌面端运行时替换)
        supported = (id == hs::ComponentId::Kernel || id == hs::ComponentId::WebUI
                     || id == hs::ComponentId::NodeRuntime)
                    && states_[row].updateAvailable;
    }
    updateBtn_->setEnabled(supported);
    if (row >= 0 && row < (int)states_.size()) {
        auto& st = states_[row];
        if (st.updateAvailable) {
            if (st.id == hs::ComponentId::NodeRuntime)
                updateBtn_->setToolTip("下载官方 Node 发行版替换便携包 node/ (点击后选择版本)");
            else
                updateBtn_->setToolTip(QString("更新 %1 到 %2").arg(
                    QString::fromStdString(st.name), QString::fromStdString(st.latestVersion)));
        } else {
            updateBtn_->setToolTip("该组件已是最新, 或暂不支持一键更新");
        }
    }
}

void UpdatesPage::setBusy(bool busy) {
    checkBtn_->setEnabled(!busy);
    remoteBtn_->setEnabled(!busy);
    checkBtn_->setText(busy ? "探测中..." : "重新探测本地");
    if (!busy) onSelectionChanged();
}

void UpdatesPage::setUpdating(bool updating, const QString& stage) {
    if (updating) {
        updateBtn_->setEnabled(false);
        progress_->setText(stage.isEmpty() ? "正在更新..." : stage);
        progress_->show();
    } else {
        progress_->setText(stage);
        progress_->show();
        onSelectionChanged();
    }
}

void UpdatesPage::appendLog(const QString& line) { Q_UNUSED(line); }
