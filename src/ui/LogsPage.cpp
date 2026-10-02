#include "LogsPage.h"
#include "../app/Paths.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QLabel>

LogsPage::LogsPage(QWidget* parent) : QWidget(parent) { buildUi(); }

void LogsPage::buildUi() {
    auto* lay = new QVBoxLayout(this);

    auto* bar = new QHBoxLayout();
    bar->addWidget(new QLabel("级别:", this));
    levelBox_ = new QComboBox(this);
    levelBox_->addItem("全部", "ALL");
    levelBox_->addItem("INFO", "INFO");
    levelBox_->addItem("WARN", "WARN");
    levelBox_->addItem("ERROR", "ERROR");
    bar->addWidget(levelBox_);
    bar->addWidget(new QLabel("搜索:", this));
    search_ = new QLineEdit(this);
    search_->setPlaceholderText("按关键字过滤...");
    search_->setClearButtonEnabled(true);
    bar->addWidget(search_, 1);
    refreshBtn_ = new QPushButton("刷新", this);
    exportBtn_  = new QPushButton("导出", this);
    clearBtn_   = new QPushButton("清空显示", this);
    bar->addWidget(refreshBtn_);
    bar->addWidget(exportBtn_);
    bar->addWidget(clearBtn_);
    lay->addLayout(bar);

    view_ = new QPlainTextEdit(this);
    view_->setReadOnly(true);
    view_->setMaximumBlockCount(20000);
    QFont f = view_->font(); f.setFamily("Consolas"); f.setPointSize(9); view_->setFont(f);
    lay->addWidget(view_, 1);

    connect(levelBox_, qOverload<int>(&QComboBox::currentIndexChanged), this, &LogsPage::applyFilter);
    connect(search_, &QLineEdit::textChanged, this, &LogsPage::applyFilter);
    connect(refreshBtn_, &QPushButton::clicked, this, &LogsPage::reload);
    connect(clearBtn_, &QPushButton::clicked, this, [this] { view_->clear(); });

    connect(exportBtn_, &QPushButton::clicked, this, [this] {
        QString path = QFileDialog::getSaveFileName(this, "导出日志", "logs_export.txt", "文本 (*.txt)");
        if (path.isEmpty()) return;
        QFile out(path);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) return;
        QTextStream ts(&out);
        for (const auto& l : filtered_) ts << l << "\n";
        out.close();
    });

    loadFromFile();
}

void LogsPage::reload() { loadFromFile(); }

void LogsPage::loadFromFile() {
    lines_.clear();
    QFile f(QString::fromStdString((hs::Paths::logsDir() / "launcher.log").string()));
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream ts(&f);
        while (!ts.atEnd()) {
            QString line = ts.readLine();
            if (!line.trimmed().isEmpty()) lines_.append(line);
        }
    }
    applyFilter();
}

bool LogsPage::matches(const QString& line) const {
    QString lvl = levelBox_->currentData().toString();
    if (lvl != "ALL") {
        // 简单启发: ERROR/WARN 关键字在行首附近
        bool isErr = line.contains("ERROR", Qt::CaseInsensitive) || line.contains("[错误]", Qt::CaseInsensitive);
        bool isWarn = line.contains("WARN", Qt::CaseInsensitive) || line.contains("警告", Qt::CaseInsensitive);
        if (lvl == "ERROR" && !isErr) return false;
        if (lvl == "WARN" && !(isErr || isWarn)) return false;
        if (lvl == "INFO" && (isErr || isWarn)) return false;
    }
    QString kw = search_->text().trimmed();
    if (!kw.isEmpty() && !line.contains(kw, Qt::CaseInsensitive)) return false;
    return true;
}

void LogsPage::applyFilter() {
    filtered_.clear();
    for (const auto& l : lines_) if (matches(l)) filtered_.append(l);
    rebuildView();
}

void LogsPage::rebuildView() {
    view_->clear();
    view_->appendPlainText(filtered_.join("\n"));
}

void LogsPage::appendLine(const QString& line) {
    lines_.append(line);
    if (matches(line)) {
        filtered_.append(line);
        view_->appendPlainText(line);
    }
}
