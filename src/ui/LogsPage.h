#pragma once

#include <QWidget>
#include <QPlainTextEdit>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>

// 日志页: 读取 <root>/studio/logs/launcher.log, 支持分级过滤 / 搜索 / 导出 / 清空
class LogsPage : public QWidget {
    Q_OBJECT
public:
    explicit LogsPage(QWidget* parent = nullptr);

    // 追加一行 (MainWindow 调用)
    void appendLine(const QString& line);

    // 从文件重新加载
    void reload();

private slots:
    void applyFilter();

private:
    void buildUi();
    void loadFromFile();
    void rebuildView();
    bool matches(const QString& line) const;

    QPlainTextEdit* view_    = nullptr;
    QComboBox*      levelBox_ = nullptr;
    QLineEdit*      search_  = nullptr;
    QPushButton*    refreshBtn_ = nullptr;
    QPushButton*    exportBtn_ = nullptr;
    QPushButton*    clearBtn_ = nullptr;

    QStringList     lines_;      // 原始行
    QStringList     filtered_;   // 过滤后行
};
