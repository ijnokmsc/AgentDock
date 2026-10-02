#include "HomePage.h"

#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QButtonGroup>
#include <QRadioButton>

HomePage::HomePage(QWidget* parent) : QWidget(parent) { buildUi(); }

void HomePage::buildUi() {
    auto* main = new QVBoxLayout(this);

    // ---- 状态卡片 ----
    auto* info = new QGroupBox("环境状态");
    auto* form = new QFormLayout(info);
    rootValue_   = new QLabel("-", this);
    kernelValue_ = new QLabel("-", this);
    webUiValue_  = new QLabel("-", this);
    modelValue_  = new QLabel("-", this);
    stateValue_  = new QLabel("未运行", this);

    rootValue_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    stateValue_->setStyleSheet("font-weight: bold;");

    form->addRow("Hermes 根目录:", rootValue_);
    form->addRow("内核版本:", kernelValue_);
    form->addRow("Web UI 版本:", webUiValue_);
    form->addRow("默认模型:", modelValue_);
    form->addRow("运行状态:", stateValue_);
    main->addWidget(info);

    // ---- 启动模式 ----
    auto* modeBox = new QGroupBox("启动模式");
    auto* modeLay = new QHBoxLayout(modeBox);
    modeCombo_ = new QComboBox(this);
    modeCombo_->addItem("终端 CLI", (int)hs::StartMode::Cli);
    modeCombo_->addItem("Web UI (:8648)", (int)hs::StartMode::WebUI);
    modeCombo_->addItem("桌面版 (Electron)", (int)hs::StartMode::Desktop);
    modeCombo_->setCurrentIndex(1);
    modeLay->addWidget(modeCombo_);
    modeLay->addStretch();
    main->addWidget(modeBox);

    // ---- 操作按钮 ----
    auto* btnLay = new QHBoxLayout();
    startBtn_   = new QPushButton("启动 Hermes", this);
    stopBtn_    = new QPushButton("停止", this);
    browserBtn_ = new QPushButton("打开 Web UI", this);
    stopBtn_->setEnabled(false);

    startBtn_->setMinimumHeight(36);
    stopBtn_->setMinimumHeight(36);

    btnLay->addWidget(startBtn_);
    btnLay->addWidget(stopBtn_);
    btnLay->addWidget(browserBtn_);
    btnLay->addStretch();
    main->addLayout(btnLay);

    blockHint_ = new QLabel("", this);
    blockHint_->setWordWrap(true);
    blockHint_->setStyleSheet("color: #b26a00;");
    main->addWidget(blockHint_);

    // ---- 日志 ----
    auto* logBox = new QGroupBox("最近输出");
    auto* logLay = new QVBoxLayout(logBox);
    logView_ = new QTextEdit(this);
    logView_->setReadOnly(true);
    logView_->setMaximumHeight(160);
    logView_->setFont(QFont("Consolas", 9));
    logLay->addWidget(logView_);
    main->addWidget(logBox);

    main->addStretch();

    connect(startBtn_, &QPushButton::clicked, this, [this] {
        emit startRequested(selectedMode());
    });
    connect(stopBtn_, &QPushButton::clicked, this, &HomePage::stopRequested);
    connect(browserBtn_, &QPushButton::clicked, this, &HomePage::openBrowserRequested);
}

void HomePage::setRootPath(const QString& v)     { rootValue_->setText(v); }
void HomePage::setKernelVersion(const QString& v){ kernelValue_->setText(v.isEmpty() ? "未探测" : v); }
void HomePage::setWebUiVersion(const QString& v) { webUiValue_->setText(v.isEmpty() ? "未安装" : v); }
void HomePage::setDefaultModel(const QString& v) { modelValue_->setText(v.isEmpty() ? "未设置" : v); }

void HomePage::setState(hs::HermesState state) {
    QString text;
    QString color;
    switch (state) {
        case hs::HermesState::Stopped:  text = "已停止"; color = "#666"; break;
        case hs::HermesState::Starting: text = "启动中"; color = "#1a73e8"; break;
        case hs::HermesState::Running:  text = "运行中"; color = "#137333"; break;
        case hs::HermesState::Stopping: text = "停止中"; color = "#b26a00"; break;
        case hs::HermesState::Failed:   text = "启动失败"; color = "#c5221f"; break;
    }
    stateValue_->setText(text);
    stateValue_->setStyleSheet(QString("font-weight: bold; color: %1;").arg(color));

    bool running = (state == hs::HermesState::Running || state == hs::HermesState::Starting);
    startBtn_->setEnabled(!running);
    stopBtn_->setEnabled(running);
}

void HomePage::setLaunchBlocked(bool blocked, const QString& reason) {
    blockHint_->setVisible(blocked);
    blockHint_->setText(blocked ? ("启动已被阻止: " + reason + " — 请到「体检」页处理") : "");
    startBtn_->setEnabled(!blocked);
}

void HomePage::appendLog(const QString& line) {
    logView_->append(line);
    logView_->ensureCursorVisible();
}

hs::StartMode HomePage::selectedMode() const {
    return (hs::StartMode)modeCombo_->currentData().toInt();
}

void HomePage::setModeAvailable(hs::StartMode mode, bool available, const QString& reason) {
    for (int i = 0; i < modeCombo_->count(); ++i) {
        if ((hs::StartMode)modeCombo_->itemData(i).toInt() == mode) {
            modeCombo_->setItemData(i, available ? QVariant() : QVariant(0), Qt::UserRole - 1);
            if (!available) {
                modeCombo_->setItemText(i, modeCombo_->itemText(i).split(" ").first() + " (不可用)");
                Q_UNUSED(reason);
            }
            return;
        }
    }
}
