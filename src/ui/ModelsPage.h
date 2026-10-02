#pragma once

#include <QWidget>
#include <QTableWidget>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>

#include "../core/ModelRegistry.h"

// 模型配置页: 只管理模型 (新增/编辑/删除/设默认)。
// 从原 ModelsPage 拆分而来。上方是 Provider 下拉筛选, 下方是模型表格与参数表单。
class ModelsPage : public QWidget {
    Q_OBJECT
public:
    explicit ModelsPage(QWidget* parent = nullptr);

    void setRegistry(hs::ModelRegistry* reg);
    void reload();

signals:
    void configChanged();
    void statusMessage(const QString& text);

private slots:
    void onProviderFilterChanged();
    void onSelectModel();
    void onSave();
    void onRemove();
    void onSetDefault();

private:
    void buildUi();
    void refreshProviders();
    void refreshModels();

    hs::ModelRegistry* reg_ = nullptr;

    QComboBox*    providerFilter_ = nullptr;
    QTableWidget* modelTable_     = nullptr;

    QLineEdit* modelIdEdit_     = nullptr;
    QLineEdit* upstreamEdit_    = nullptr;
    QLineEdit* displayNameEdit_ = nullptr;
    QLineEdit* temperatureEdit_ = nullptr;
    QLineEdit* maxTokensEdit_   = nullptr;
    QLineEdit* contextEdit_     = nullptr;

    QString selectedId_;
    QString providerFilterId_;   // "all" 表示全部
};
