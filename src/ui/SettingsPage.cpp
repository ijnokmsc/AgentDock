#include "SettingsPage.h"
#include "../app/Paths.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QFileDialog>
#include <QMessageBox>

SettingsPage::SettingsPage(QWidget* parent) : QWidget(parent) { buildUi(); }

void SettingsPage::buildUi() {
    auto* lay = new QVBoxLayout(this);

    // ---- 网络 / 代理 ----
    auto* netBox = new QGroupBox("网络与代理", this);
    auto* netF = new QFormLayout(netBox);
    proxyMode_ = new QComboBox(this);
    proxyMode_->addItem("系统代理", (int)hs::ProxyMode::System);
    proxyMode_->addItem("直连",     (int)hs::ProxyMode::Direct);
    proxyMode_->addItem("手动",     (int)hs::ProxyMode::Manual);
    netF->addRow("代理模式:", proxyMode_);

    proxyHost_ = new QLineEdit(this);
    proxyHost_->setPlaceholderText("如 127.0.0.1");
    proxyPort_ = new QSpinBox(this);
    proxyPort_->setRange(1, 65535);
    proxyPort_->setValue(7897);
    auto* hostPort = new QHBoxLayout();
    hostPort->addWidget(proxyHost_, 1);
    hostPort->addWidget(proxyPort_);
    netF->addRow("代理主机/端口:", hostPort);
    lay->addWidget(netBox);

    // ---- 运行与更新 ----
    auto* runBox = new QGroupBox("运行与更新", this);
    auto* runF = new QFormLayout(runBox);
    portSpin_ = new QSpinBox(this);
    portSpin_->setRange(1024, 65535);
    portSpin_->setValue(8648);
    runF->addRow("Web UI 端口:", portSpin_);

    channelBox_ = new QComboBox(this);
    channelBox_->addItem("stable", "stable");
    channelBox_->addItem("beta", "beta");
    runF->addRow("更新通道:", channelBox_);

    autoCheck_ = new QCheckBox("启动时自动检查更新", this);
    runF->addRow("", autoCheck_);
    launchOnStart_ = new QCheckBox("启动器启动时同时启动 Hermes", this);
    runF->addRow("", launchOnStart_);

    backupKeep_ = new QSpinBox(this);
    backupKeep_->setRange(1, 10);
    backupKeep_->setValue(3);
    runF->addRow("保留备份份数:", backupKeep_);
    lay->addWidget(runBox);

    // ---- 路径 ----
    auto* pathBox = new QGroupBox("Hermes 根目录", this);
    auto* pathLay = new QHBoxLayout(pathBox);
    hermesRootEdit_ = new QLineEdit(this);
    hermesRootEdit_->setPlaceholderText(".\Hermes  (相对便携包根目录)");
    auto* browseBtn = new QPushButton("浏览...", this);
    connect(browseBtn, &QPushButton::clicked, this, [this] {
        // 以当前便携包根为起始目录
        QString start = hs::Paths::portableRoot().string().c_str();
        QString dir = QFileDialog::getExistingDirectory(this, "选择 Hermes 便携包根目录", start);
        if (!dir.isEmpty()) {
            // 转为相对便携包根的相对路径
            fs::path portable = hs::Paths::portableRoot();
            fs::path sel = dir.toStdWString();
            std::error_code ec;
            auto rel = fs::relative(sel, portable, ec);
            hermesRootEdit_->setText(QString::fromStdString(rel.string()));
        }
    });
    pathLay->addWidget(hermesRootEdit_, 1);
    pathLay->addWidget(browseBtn);
    lay->addWidget(pathBox);

    auto* saveBtn = new QPushButton("保存设置", this);
    saveBtn->setFixedWidth(140);
    connect(saveBtn, &QPushButton::clicked, this, &SettingsPage::onSave);
    lay->addWidget(saveBtn, 0, Qt::AlignLeft);

    lay->addStretch();
    connect(proxyMode_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &SettingsPage::onProxyModeChanged);
}

void SettingsPage::setSettings(hs::AppSettings* s) { settings_ = s; }

void SettingsPage::onProxyModeChanged() {
    bool manual = proxyMode_->currentData().toInt() == (int)hs::ProxyMode::Manual;
    proxyHost_->setEnabled(manual);
    proxyPort_->setEnabled(manual);
}

void SettingsPage::reload() {
    if (!settings_) return;
    proxyMode_->setCurrentIndex(proxyMode_->findData((int)settings_->network.mode));
    proxyHost_->setText(QString::fromStdString(settings_->network.host));
    proxyPort_->setValue(settings_->network.port > 0 ? settings_->network.port : 7897);
    portSpin_->setValue(settings_->runtime.webUiPort);
    channelBox_->setCurrentIndex(channelBox_->findData(
        QString::fromStdString(settings_->runtime.updateChannel)));
    autoCheck_->setChecked(settings_->runtime.autoCheckUpdate);
    launchOnStart_->setChecked(settings_->runtime.launchOnStart);
    backupKeep_->setValue(settings_->runtime.backupKeepCount);
    hermesRootEdit_->setText(QString::fromStdString(settings_->runtime.hermesRoot));
    onProxyModeChanged();
}

void SettingsPage::onSave() {
    if (!settings_) { emit statusMessage("设置未初始化"); return; }
    settings_->network.mode = (hs::ProxyMode)proxyMode_->currentData().toInt();
    settings_->network.host = proxyHost_->text().trimmed().toStdString();
    settings_->network.port = proxyPort_->value();
    settings_->runtime.webUiPort = portSpin_->value();
    settings_->runtime.updateChannel = channelBox_->currentData().toString().toStdString();
    settings_->runtime.autoCheckUpdate = autoCheck_->isChecked();
    settings_->runtime.launchOnStart = launchOnStart_->isChecked();
    settings_->runtime.backupKeepCount = backupKeep_->value();
    if (!hermesRootEdit_->text().isEmpty())
        settings_->runtime.hermesRoot = hermesRootEdit_->text().toStdString();

    emit settingsSaved();
    emit statusMessage("设置已保存");
}
