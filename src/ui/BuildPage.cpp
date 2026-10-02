#include "BuildPage.h"

#include "../app/Paths.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QFileDialog>
#include <QFontDatabase>
#include <QDesktopServices>
#include <QUrl>

BuildPage::BuildPage(QWidget* parent) : QWidget(parent) {
    buildUi();
}

void BuildPage::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(10);

    auto* title = new QLabel("构建便携版", this);
    QFont tf = title->font();
    tf.setPointSize(tf.pointSize() + 3);
    tf.setBold(true);
    title->setFont(tf);

    auto* desc = new QLabel(
        "只从官方 releases 拉两样东西 —— Hermes runtime（python/node/git/内核源码）"
        "与 web-ui 包 —— 解包 + 写启动脚本 + 建立沙箱，组装成一个完整、可搬走的便携版。\n"
        "不依赖任何第三方打包好的便携包。首次构建需下载约 670 MB。", this);
    desc->setWordWrap(true);
    desc->setStyleSheet("color: #666;");

    root->addWidget(title);
    root->addWidget(desc);

    // ---- 目标目录 ----
    auto* dirBox = new QGroupBox("目标目录", this);
    auto* dirLay = new QHBoxLayout(dirBox);
    target_ = new QLineEdit(dirBox);
    target_->setPlaceholderText("留空 = 启动器所在目录（推荐：直接原位构建/更新）");
    browse_ = new QPushButton("浏览...", dirBox);
    openDir_ = new QPushButton("打开", dirBox);
    dirLay->addWidget(target_, 1);
    dirLay->addWidget(browse_);
    dirLay->addWidget(openDir_);
    root->addWidget(dirBox);

    auto* dirHint = new QLabel(
        "目标目录会与启动器同级：构建产物是 python/ node/ webui/ git/ data/ _home/ "
        "以及 Hermes.bat。覆盖已有便携包时只覆盖程序文件，不动你的 studio/ 配置与 data/ 会话。", this);
    dirHint->setWordWrap(true);
    dirHint->setStyleSheet("color: #888; font-size: 11px;");
    root->addWidget(dirHint);

    // ---- 版本 ----
    auto* verBox = new QGroupBox("官方版本", this);
    auto* verLay = new QVBoxLayout(verBox);
    versions_ = new QLabel("尚未检查（点「检查官方版本」会查 GitHub releases）", verBox);
    versions_->setWordWrap(true);
    resolve_ = new QPushButton("检查官方版本", verBox);
    resolve_->setFixedWidth(140);
    auto* verRow = new QHBoxLayout();
    verRow->addWidget(versions_, 1);
    verRow->addWidget(resolve_);
    verLay->addLayout(verRow);
    root->addWidget(verBox);

    // ---- 动作 ----
    auto* actRow = new QHBoxLayout();
    build_  = new QPushButton("开始构建", this);
    cancel_ = new QPushButton("取消", this);
    cancel_->setEnabled(false);
    actRow->addWidget(build_);
    actRow->addWidget(cancel_);
    actRow->addStretch(1);
    root->addLayout(actRow);

    // ---- 进度 ----
    bar_ = new QProgressBar(this);
    bar_->setRange(0, 100);
    bar_->setValue(0);
    bar_->setFormat("%p%");
    stage_ = new QLabel("就绪", this);
    stage_->setStyleSheet("color: #444;");
    root->addWidget(bar_);
    root->addWidget(stage_);

    // ---- 日志 ----
    log_ = new QPlainTextEdit(this);
    log_->setReadOnly(true);
    log_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    log_->setMinimumHeight(160);
    root->addWidget(log_, 1);

    connect(browse_,   &QPushButton::clicked, this, &BuildPage::onBrowse);
    connect(resolve_,  &QPushButton::clicked, this, &BuildPage::resolveRequested);
    connect(build_,    &QPushButton::clicked, this, &BuildPage::buildRequested);
    connect(cancel_,   &QPushButton::clicked, this, &BuildPage::cancelRequested);
    connect(openDir_,  &QPushButton::clicked, this, [this] {
        QString d = targetDir();
        if (d.isEmpty()) d = QString::fromStdString(hs::Paths::portableRoot().string());
        if (!d.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(d));
    });

    // 默认目标 = 启动器所在目录 (原位构建)
    const std::string pr = hs::Paths::portableRoot().string();
    if (!pr.empty()) target_->setText(QString::fromStdString(pr));
}

void BuildPage::onBrowse() {
    const QString start = targetDir().isEmpty()
        ? QString::fromStdString(hs::Paths::portableRoot().string())
        : targetDir();
    const QString dir = QFileDialog::getExistingDirectory(this, "选择便携版目标目录", start);
    if (!dir.isEmpty()) target_->setText(dir);
}

void BuildPage::setTargetDir(const QString& dir) { target_->setText(dir); }

QString BuildPage::targetDir() const { return target_->text().trimmed(); }

QString BuildPage::mirror() const { return QString(); }   // 预留: 镜像前缀

void BuildPage::setBusy(bool busy) {
    build_->setEnabled(!busy);
    resolve_->setEnabled(!busy);
    browse_->setEnabled(!busy);
    target_->setEnabled(!busy);
    cancel_->setEnabled(busy);
}

void BuildPage::setResolved(const QString& runtimeVer, const QString& webUiVer, const QString& note) {
    if (runtimeVer.isEmpty() && webUiVer.isEmpty()) {
        versions_->setText(note);
        return;
    }
    versions_->setText(QString("Hermes runtime: %1　·　web-ui: %2%3")
                           .arg(runtimeVer.isEmpty() ? "-" : runtimeVer,
                                webUiVer.isEmpty() ? "-" : webUiVer,
                                note.isEmpty() ? "" : "　(" + note + ")"));
}

void BuildPage::setProgress(int percent, const QString& stage) {
    bar_->setValue(percent);
    if (!stage.isEmpty()) stage_->setText(stage);
}

void BuildPage::appendLog(const QString& line) {
    log_->appendPlainText(line);
}

void BuildPage::setResult(bool ok, const QString& message) {
    appendLog((ok ? "[完成] " : "[失败] ") + message);
    stage_->setText(message);
}

void BuildPage::setHasPackage(bool hasPackage) {
    stage_->setText(hasPackage ? "目标目录已存在一个便携包（构建会覆盖程序文件，保留配置与会话）"
                               : "目标目录还是空的（将是全新建构）");
}
