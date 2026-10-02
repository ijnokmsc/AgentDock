#pragma once

#include <QWidget>
#include <QTableWidget>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>

#include "../core/ModelRegistry.h"

// Provider 配置页: 只管理模型供应商 (新增/编辑/删除/启停)。
// 从原 ModelsPage 拆分而来, 解决"模型配置太拥挤"的问题。
class ProvidersPage : public QWidget {
    Q_OBJECT
public:
    explicit ProvidersPage(QWidget* parent = nullptr);

    void setRegistry(hs::ModelRegistry* reg);
    void reload();

signals:
    void configChanged();
    void statusMessage(const QString& text);

private slots:
    void onSelect();
    void onSave();
    void onRemove();
    void onToggleEnabled();

private:
    void buildUi();
    void refresh();

    hs::ModelRegistry* reg_ = nullptr;

    QTableWidget* table_ = nullptr;
    QLineEdit*    idEdit_     = nullptr;
    QLineEdit*    nameEdit_   = nullptr;
    QLineEdit*    baseUrlEdit_= nullptr;
    QLineEdit*    envVarEdit_ = nullptr;
    QLineEdit*    apiKeyEdit_ = nullptr;
    QComboBox*    kindCombo_  = nullptr;
    QPushButton*  enabledBtn_ = nullptr;

    QString selectedId_;
    bool    editingExisting_ = false;
};
