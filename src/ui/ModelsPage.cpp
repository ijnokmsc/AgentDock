#include "ModelsPage.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>

ModelsPage::ModelsPage(QWidget* parent) : QWidget(parent) { buildUi(); }

void ModelsPage::buildUi() {
    auto* lay = new QVBoxLayout(this);

    // ---- 顶部: Provider 筛选 ----
    auto* filterRow = new QHBoxLayout();
    auto* filterLabel = new QLabel("Provider:", this);
    providerFilter_ = new QComboBox(this);
    providerFilter_->addItem("全部 Provider", "all");
    filterRow->addWidget(filterLabel);
    filterRow->addWidget(providerFilter_, 1);
    lay->addLayout(filterRow);

    // ---- 模型表格 ----
    auto* box = new QGroupBox("模型列表");
    auto* v = new QVBoxLayout(box);

    modelTable_ = new QTableWidget(this);
    modelTable_->setColumnCount(5);
    modelTable_->setHorizontalHeaderLabels({"模型 ID", "Provider", "上游 ID", "显示名称", "默认"});
    modelTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    modelTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    modelTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    modelTable_->setAlternatingRowColors(true);
    modelTable_->horizontalHeader()->setStretchLastSection(true);
    modelTable_->verticalHeader()->setVisible(false);
    v->addWidget(modelTable_);

    auto* f = new QFormLayout();
    modelIdEdit_     = new QLineEdit(this);
    upstreamEdit_    = new QLineEdit(this);
    displayNameEdit_ = new QLineEdit(this);
    temperatureEdit_ = new QLineEdit(this);
    maxTokensEdit_   = new QLineEdit(this);
    contextEdit_     = new QLineEdit(this);
    temperatureEdit_->setPlaceholderText("如 0.3");
    maxTokensEdit_->setPlaceholderText("如 8192");
    contextEdit_->setPlaceholderText("如 131072");

    f->addRow("模型 ID (唯一):", modelIdEdit_);
    f->addRow("上游模型 ID:", upstreamEdit_);
    f->addRow("显示名称:", displayNameEdit_);
    f->addRow("Temperature:", temperatureEdit_);
    f->addRow("Max Tokens:", maxTokensEdit_);
    f->addRow("上下文窗口:", contextEdit_);
    v->addLayout(f);

    auto* btn = new QHBoxLayout();
    auto* saveBtn = new QPushButton("新增 / 保存模型", this);
    auto* delBtn  = new QPushButton("删除模型", this);
    auto* defBtn  = new QPushButton("设为默认", this);
    btn->addWidget(saveBtn);
    btn->addWidget(delBtn);
    btn->addWidget(defBtn);
    btn->addStretch();
    v->addLayout(btn);
    lay->addWidget(box);

    auto* note = new QLabel(
        "模型绑定到 Provider (见 Provider 页)。新增模型前先在顶部选择其所属 Provider。"
        "若选『全部 Provider』, 新增的模型归属当前表格选中行所属的 Provider。"
        "保存后自动渲染到 data/.env 与 data/config.yaml。", this);
    note->setWordWrap(true);
    note->setStyleSheet("color: #555; padding: 4px;");
    lay->addWidget(note);

    lay->addStretch();

    connect(providerFilter_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &ModelsPage::onProviderFilterChanged);
    connect(modelTable_, &QTableWidget::itemSelectionChanged, this, &ModelsPage::onSelectModel);
    connect(saveBtn, &QPushButton::clicked, this, &ModelsPage::onSave);
    connect(delBtn, &QPushButton::clicked, this, &ModelsPage::onRemove);
    connect(defBtn, &QPushButton::clicked, this, &ModelsPage::onSetDefault);
}

void ModelsPage::setRegistry(hs::ModelRegistry* reg) { reg_ = reg; }

void ModelsPage::reload() { refreshProviders(); refreshModels(); }

void ModelsPage::refreshProviders() {
    if (!reg_) return;
    // 记住当前筛选
    QString cur = providerFilter_->currentData().toString();
    providerFilter_->blockSignals(true);
    providerFilter_->clear();
    providerFilter_->addItem("全部 Provider", "all");
    for (const auto& p : reg_->providers()) {
        providerFilter_->addItem(QString::fromStdString(p.displayName + " (" + p.id + ")"),
                                 QString::fromStdString(p.id));
    }
    int idx = providerFilter_->findData(cur);
    if (idx < 0) idx = 0;
    providerFilter_->setCurrentIndex(idx);
    providerFilter_->blockSignals(false);
    providerFilterId_ = providerFilter_->currentData().toString();
}

void ModelsPage::refreshModels() {
    if (!reg_) return;
    modelTable_->setRowCount(0);
    // 收集当前筛选下的模型
    std::vector<hs::Model> ms;
    if (providerFilterId_ == "all") {
        for (const auto& m : reg_->models()) ms.push_back(m);
    } else {
        for (const auto& m : reg_->modelsOf(providerFilterId_.toStdString())) ms.push_back(m);
    }
    for (const auto& m : ms) {
        int row = modelTable_->rowCount();
        modelTable_->insertRow(row);
        modelTable_->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(m.id)));
        modelTable_->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(m.providerId)));
        modelTable_->setItem(row, 2, new QTableWidgetItem(QString::fromStdString(m.upstreamId)));
        modelTable_->setItem(row, 3, new QTableWidgetItem(QString::fromStdString(m.displayName)));
        auto* d = new QTableWidgetItem(m.isDefault ? "默认" : "");
        if (m.isDefault) { d->setForeground(QColor("#137333")); QFont f = d->font(); f.setBold(true); d->setFont(f); }
        modelTable_->setItem(row, 4, d);
    }
}

void ModelsPage::onProviderFilterChanged() {
    providerFilterId_ = providerFilter_->currentData().toString();
    refreshModels();
}

void ModelsPage::onSelectModel() {
    int row = modelTable_->currentRow();
    if (row < 0 || !reg_) return;
    std::vector<hs::Model> ms;
    if (providerFilterId_ == "all") ms = reg_->models();
    else ms = reg_->modelsOf(providerFilterId_.toStdString());
    if (row >= (int)ms.size()) return;
    const auto& m = ms[row];
    selectedId_ = QString::fromStdString(m.id);

    modelIdEdit_->setText(QString::fromStdString(m.id));
    upstreamEdit_->setText(QString::fromStdString(m.upstreamId));
    displayNameEdit_->setText(QString::fromStdString(m.displayName));
    temperatureEdit_->clear();
    maxTokensEdit_->clear();
    contextEdit_->clear();
    if (m.params.temperature) temperatureEdit_->setText(QString::number(*m.params.temperature));
    if (m.params.maxTokens)   maxTokensEdit_->setText(QString::number(*m.params.maxTokens));
    if (m.params.contextWindow) contextEdit_->setText(QString::number(*m.params.contextWindow));
}

void ModelsPage::onSave() {
    if (!reg_) return;
    QString id = modelIdEdit_->text().trimmed();
    if (id.isEmpty()) { emit statusMessage("模型 ID 不能为空"); return; }

    // 归属 Provider: 优先用选中行的 Provider, 否则用当前筛选的 Provider
    QString providerId;
    int row = modelTable_->currentRow();
    if (providerFilterId_ == "all" && row >= 0) {
        auto ms = reg_->models();
        if (row < (int)ms.size()) providerId = QString::fromStdString(ms[row].providerId);
    } else {
        providerId = providerFilterId_;
    }
    if (providerId.isEmpty() || providerId == "all") {
        emit statusMessage("请先选择要归属的 Provider (顶部下拉或选中某模型)");
        return;
    }

    hs::Model m;
    m.id          = id.toStdString();
    m.providerId  = providerId.toStdString();
    m.upstreamId  = upstreamEdit_->text().trimmed().toStdString();
    m.displayName = displayNameEdit_->text().trimmed().toStdString();
    if (m.upstreamId.empty()) m.upstreamId = m.id;
    if (m.displayName.empty()) m.displayName = m.id;

    bool okDouble = false, okInt = false;
    double temp = temperatureEdit_->text().toDouble(&okDouble);
    int    mt   = maxTokensEdit_->text().toInt(&okInt);
    bool   okCtx = false;
    int    ctx  = contextEdit_->text().toInt(&okCtx);
    if (okDouble) m.params.temperature = temp;
    if (okInt)    m.params.maxTokens = mt;
    if (okCtx)    m.params.contextWindow = ctx;

    auto r = reg_->model(m.id) ? reg_->updateModel(m) : reg_->addModel(m);
    if (!r.ok()) { emit statusMessage(QString::fromStdString(r.error->message)); return; }

    refreshModels();
    emit statusMessage("模型已保存: " + id);
    emit configChanged();
}

void ModelsPage::onRemove() {
    int row = modelTable_->currentRow();
    if (row < 0 || !reg_) return;
    std::vector<hs::Model> ms;
    if (providerFilterId_ == "all") ms = reg_->models();
    else ms = reg_->modelsOf(providerFilterId_.toStdString());
    if (row >= (int)ms.size()) return;
    QString id = QString::fromStdString(ms[row].id);

    auto r = reg_->removeModel(id.toStdString());
    if (!r.ok()) { emit statusMessage(QString::fromStdString(r.error->message)); return; }

    refreshModels();
    emit statusMessage("已删除模型: " + id);
    emit configChanged();
}

void ModelsPage::onSetDefault() {
    int row = modelTable_->currentRow();
    if (row < 0 || !reg_) return;
    std::vector<hs::Model> ms;
    if (providerFilterId_ == "all") ms = reg_->models();
    else ms = reg_->modelsOf(providerFilterId_.toStdString());
    if (row >= (int)ms.size()) return;

    auto r = reg_->setDefault(ms[row].id);
    if (!r.ok()) { emit statusMessage(QString::fromStdString(r.error->message)); return; }

    refreshModels();
    emit statusMessage("已设为默认模型: " + QString::fromStdString(ms[row].displayName));
    emit configChanged();
}
