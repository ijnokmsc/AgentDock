#pragma once

#include <QWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QProgressBar>
#include <QPlainTextEdit>

// 构建页: 一键从官方 releases 组装完整便携版 (ADR-004)。
//
// 页面只负责展示与发信号; 真正的网络/解包/写盘在 MainWindow 里编排到后台线程,
// 结果再回灌到本页 (setResolved / setProgress / appendLog / setResult)。
class BuildPage : public QWidget {
    Q_OBJECT
public:
    explicit BuildPage(QWidget* parent = nullptr);

    void setTargetDir(const QString& dir);
    QString targetDir() const;
    QString mirror() const;

    void setBusy(bool busy);
    void setResolved(const QString& runtimeVer, const QString& webUiVer, const QString& note);
    void setProgress(int percent, const QString& stage);
    void appendLog(const QString& line);
    void setResult(bool ok, const QString& message);
    void setHasPackage(bool hasPackage);   // 目标目录是否已是一个可用便携包

signals:
    void resolveRequested();
    void buildRequested();
    void cancelRequested();
    void openTargetRequested();

private slots:
    void onBrowse();

private:
    void buildUi();

    QLineEdit*      target_    = nullptr;
    QPushButton*    browse_    = nullptr;
    QPushButton*    resolve_   = nullptr;
    QPushButton*    build_     = nullptr;
    QPushButton*    cancel_    = nullptr;
    QPushButton*    openDir_   = nullptr;
    QLabel*         versions_  = nullptr;
    QLabel*         stage_     = nullptr;
    QProgressBar*   bar_       = nullptr;
    QPlainTextEdit* log_       = nullptr;
};
