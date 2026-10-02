#include "ProvidersPage.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>

ProvidersPage::ProvidersPage(QWidget* parent) : QWidget(parent) { buildUi(); }

void ProvidersPage::buildUi() {
    auto* lay = new QVBoxLayout(this);

    auto* box = new QGroupBox("模型供应商 (Provider)");
    auto* v = new QVBoxLayout(box);

    table_ = new QTableWidget(this);
    table_->setColumnCount(5);
    table_->setHorizontalHeaderLabels({"ID", "名称", "类型", "Base URL", "状态"});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->verticalHeader()->setVisible(false);
    v->addWidget(table_);

    auto* f = new QFormLayout();
    idEdit_      = new QLineEdit(this);
    nameEdit_    = new QLineEdit(this);
    baseUrlEdit_ = new QLineEdit(this);
    envVarEdit_  = new QLineEdit(this);
    apiKeyEdit_  = new QLineEdit(this);
    kindCombo_   = new QComboBox(this);
    kindCombo_->addItem("OpenAI 兼容", "openai-compatible");
    kindCombo_->addItem("OpenAI", "openai");
    kindCombo_->addItem("Anthropic", "anthropic");
    kindCombo_->addItem("Ollama", "ollama");
    apiKeyEdit_->setEchoMode(QLineEdit::Password);
    apiKeyEdit_->setPlaceholderText("留空则保持原值不变");

    f->addRow("ID (英文, 如 deepseek):", idEdit_);
    f->addRow("显示名称:", nameEdit_);
    f->addRow("Base URL (可选):", baseUrlEdit_);
    f->addRow("环境变量名:", envVarEdit_);
    f->addRow("API Key:", apiKeyEdit_);
    f->addRow("类型:", kindCombo_);
    v->addLayout(f);

    auto* btn = new QHBoxLayout();
    auto* saveBtn = new QPushButton("新增 / 保存 Provider", this);
    auto* delBtn  = new QPushButton("删除 Provider", this);
    enabledBtn_   = new QPushButton(this);
    enabledBtn_->setEnabled(false);
    btn->addWidget(saveBtn);
    btn->addWidget(delBtn);
    btn->addWidget(enabledBtn_);
    btn->addStretch();
    v->addLayout(btn);
    lay->addWidget(box);

    auto* note = new QLabel(
        "Provider 决定连接哪家模型服务。删除 Provider 会连带删除其下的全部模型。"
        "保存后自动渲染到 data/.env 与 data/config.yaml。", this);
    note->setWordWrap(true);
    note->setStyleSheet("color: #555; padding: 4px;");
    lay->addWidget(note);

    lay->addStretch();

    connect(table_, &QTableWidget::itemSelectionChanged, this, &ProvidersPage::onSelect);
    connect(saveBtn, &QPushButton::clicked, this, &ProvidersPage::onSave);
    connect(delBtn, &QPushButton::clicked, this, &ProvidersPage::onRemove);
    connect(enabledBtn_, &QPushButton::clicked, this, &ProvidersPage::onToggleEnabled);
}

void ProvidersPage::setRegistry(hs::ModelRegistry* reg) { reg_ = reg; }

void ProvidersPage::reload() { refresh(); }

void ProvidersPage::refresh() {
    if (!reg_) return;
    table_->setRowCount(0);
    for (const auto& p : reg_->providers()) {
        int row = table_->rowCount();
        table_->insertRow(row);
        table_->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(p.id)));
        table_->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(p.displayName)));
        table_->setItem(row, 2, new QTableWidgetItem(QString::fromStdString(p.kind)));
        table_->setItem(row, 3, new QTableWidgetItem(QString::fromStdString(p.baseUrl)));
        auto* st = new QTableWidgetItem(p.enabled ? "已启用" : "已停用");
        st->setForeground(p.enabled ? QColor("#137333") : QColor("#999"));
        table_->setItem(row, 4, st);
    }
    if (table_->rowCount() > 0 && table_->currentRow() < 0) table_->selectRow(0);
}

void ProvidersPage::onSelect() {
    int row = table_->currentRow();
    if (row < 0 || !reg_) return;
    auto ps = reg_->providers();
    if (row >= (int)ps.size()) return;
    const auto& p = ps[row];
    selectedId_ = QString::fromStdString(p.id);
    editingExisting_ = true;

    idEdit_->setText(QString::fromStdString(p.id));
    nameEdit_->setText(QString::fromStdString(p.displayName));
    baseUrlEdit_->setText(QString::fromStdString(p.baseUrl));
    envVarEdit_->setText(QString::fromStdString(p.envVar));
    apiKeyEdit_->clear();
    int idx = kindCombo_->findData(QString::fromStdString(p.kind));
    if (idx >= 0) kindCombo_->setCurrentIndex(idx);
    enabledBtn_->setEnabled(true);
    enabledBtn_->setText(p.enabled ? "点击停用" : "点击启用");
}

void ProvidersPage::onSave() {
    if (!reg_) return;
    QString id = idEdit_->text().trimmed();
    if (id.isEmpty()) { emit statusMessage("Provider ID 不能为空"); return; }

    hs::Provider p;
    p.id          = id.toStdString();
    p.displayName = nameEdit_->text().trimmed().toStdString();
    p.baseUrl     = baseUrlEdit_->text().trimmed().toStdString();
    p.envVar      = envVarEdit_->text().trimmed().toStdString();
    p.kind        = kindCombo_->currentData().toString().toStdString();
    p.apiKey      = apiKeyEdit_->text().toStdString();
    if (p.envVar.empty()) {
        QString upper = id.toUpper();
        p.envVar = (upper + "_API_KEY").toStdString();
    }
    // 保留原 enabled 状态
    if (auto old = reg_->provider(p.id)) p.enabled = old->enabled;

    auto r = reg_->provider(p.id) ? reg_->updateProvider(p) : reg_->addProvider(p);
    if (!r.ok()) { emit statusMessage(QString::fromStdString(r.error->message)); return; }

    selectedId_ = id;
    editingExisting_ = true;
    refresh();
    emit statusMessage("Provider 已保存: " + id);
    emit configChanged();
}

void ProvidersPage::onRemove() {
    int row = table_->currentRow();
    if (row < 0 || !reg_) return;
    auto ps = reg_->providers();
    if (row >= (int)ps.size()) return;
    QString id = QString::fromStdString(ps[row].id);

    if (QMessageBox::question(this, "确认删除",
            QString("删除 Provider \"%1\" 会连带删除其下的全部模型, 确定吗?").arg(id))
        != QMessageBox::Yes) return;

    auto r = reg_->removeProvider(id.toStdString());
    if (!r.ok()) { emit statusMessage(QString::fromStdString(r.error->message)); return; }

    selectedId_.clear();
    editingExisting_ = false;
    refresh();
    emit statusMessage("已删除 Provider: " + id);
    emit configChanged();
}

void ProvidersPage::onToggleEnabled() {
    if (selectedId_.isEmpty() || !reg_) return;
    auto p = reg_->provider(selectedId_.toStdString());
    if (!p) return;
    p->enabled = !p->enabled;
    auto r = reg_->updateProvider(*p);
    if (!r.ok()) { emit statusMessage(QString::fromStdString(r.error->message)); return; }
    refresh();
    onSelect();
    emit statusMessage(QString::fromStdString(p->id) + (p->enabled ? " 已启用" : " 已停用"));
    emit configChanged();
}
