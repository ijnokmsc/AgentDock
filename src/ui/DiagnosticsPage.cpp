#include "DiagnosticsPage.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>

DiagnosticsPage::DiagnosticsPage(QWidget* parent) : QWidget(parent) { buildUi(); }

void DiagnosticsPage::buildUi() {
    auto* lay = new QVBoxLayout(this);

    summary_ = new QLabel("尚未体检", this);
    summary_->setStyleSheet("font-weight: bold; padding: 4px;");
    lay->addWidget(summary_);

    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(3);
    tree_->setHeaderLabels({"状态", "检查项", "详情"});
    tree_->setRootIsDecorated(false);
    tree_->setAlternatingRowColors(true);
    tree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tree_->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    lay->addWidget(tree_);

    auto* btnLay = new QHBoxLayout();
    recheckBtn_ = new QPushButton("重新检查", this);
    fixAllBtn_  = new QPushButton("修复可修复项", this);
    btnLay->addWidget(recheckBtn_);
    btnLay->addWidget(fixAllBtn_);
    btnLay->addStretch();
    lay->addLayout(btnLay);

    connect(recheckBtn_, &QPushButton::clicked, this, &DiagnosticsPage::recheckRequested);
    connect(fixAllBtn_, &QPushButton::clicked, this, [this] {
        // 逐个发出可自动修复项的 action
        for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
            QTreeWidgetItem* it = tree_->topLevelItem(i);
            QString action = it->data(0, Qt::UserRole).toString();
            if (!action.isEmpty()) emit fixRequested(action);
        }
    });
}

void DiagnosticsPage::setItems(const QVector<hs::CheckItem>& items) {
    tree_->clear();

    int fails = 0, warns = 0;
    for (const auto& c : items) {
        auto* it = new QTreeWidgetItem(tree_);
        QString mark, color;
        switch (c.level) {
            case hs::CheckLevel::Pass: mark = "PASS"; color = "#137333"; ++warns; break;
            case hs::CheckLevel::Warn: mark = "WARN"; color = "#b26a00"; ++warns; break;
            case hs::CheckLevel::Fail: mark = "FAIL"; color = "#c5221f"; ++fails; break;
        }
        it->setText(0, mark);
        it->setForeground(0, QColor(color));
        it->setText(1, QString::fromStdString(c.title));
        it->setText(2, QString::fromStdString(c.detail));
        // 只有 auto: 开头才是可自动修复
        QString action = QString::fromStdString(c.fixAction);
        it->setData(0, Qt::UserRole, action.startsWith("auto:") ? action : QString());
        if (!action.isEmpty() && !action.startsWith("auto:")) {
            it->setText(2, it->text(2) + "   [建议: " + action + "]");
        }
    }
    Q_UNUSED(warns);

    if (fails > 0) {
        summary_->setText(QString("发现 %1 项阻断问题, 修复后才能启动").arg(fails));
        summary_->setStyleSheet("font-weight: bold; color: #c5221f; padding: 4px;");
    } else {
        summary_->setText("全部检查通过, 可以启动");
        summary_->setStyleSheet("font-weight: bold; color: #137333; padding: 4px;");
    }
}

void DiagnosticsPage::setSummary(const QString& text) { summary_->setText(text); }
