#pragma once

#include <QWidget>
#include <QLineEdit>
#include <QSpinBox>
#include <QComboBox>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>

#include "../core/Types.h"

// 设置页: 代理(三态) / web-ui 端口 / 更新通道 / 路径 / 更新行为
// 数据读写 hs::AppSettings (保存到 <root>/studio/settings.json)
class SettingsPage : public QWidget {
    Q_OBJECT
public:
    explicit SettingsPage(QWidget* parent = nullptr);

    void setSettings(hs::AppSettings* s);
    void reload();

signals:
    void statusMessage(const QString& text);
    void settingsSaved();

private slots:
    void onProxyModeChanged();
    void onSave();

private:
    void buildUi();

    hs::AppSettings* settings_ = nullptr;

    // 代理
    QComboBox* proxyMode_  = nullptr;
    QLineEdit* proxyHost_  = nullptr;
    QSpinBox*  proxyPort_  = nullptr;
    // 端口 / 通道 / 行为
    QSpinBox*  portSpin_   = nullptr;
    QComboBox* channelBox_ = nullptr;
    QCheckBox* autoCheck_  = nullptr;
    QCheckBox* launchOnStart_ = nullptr;
    QSpinBox*  backupKeep_ = nullptr;
    // 路径
    QLineEdit* hermesRootEdit_ = nullptr;
};
