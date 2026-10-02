#pragma once

#include <QMainWindow>
#include <QListWidget>
#include <QStackedWidget>
#include <QLabel>
#include <QPushButton>
#include <QFutureWatcher>
#include <QSystemTrayIcon>
#include <QMenu>

#include <memory>
#include <vector>

#include "../core/Types.h"
#include "../core/ModelRegistry.h"
#include "../core/ConfigRenderer.h"
#include "../core/PreflightChecker.h"
#include "../core/Launcher.h"
#include "../core/Updater.h"
#include "../core/PortableBuilder.h"
#include "../core/PluginHost.h"

#include <atomic>

class HomePage;
class DiagnosticsPage;
class ProvidersPage;
class ModelsPage;
class UpdatesPage;
class PluginsPage;
class SettingsPage;
class LogsPage;
class BuildPage;

// 主窗口: 左侧导航 + 右侧页面栈 + 底部状态条。
//
// 所有 Core 调用都在这里编排; 页面只负责展示与发信号。
// 耗时操作 (版本探测 / 体检 / 渲染) 走 QtConcurrent, 结果回主线程。
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // 供页面访问的核心对象
    hs::ModelRegistry*   registry()   { return registry_.get(); }
    hs::Launcher*        launcher()   { return launcher_.get(); }
    hs::Updater*         updater()    { return updater_.get(); }
    hs::RuntimeConfig&   runtime()    { return settings_.runtime; }
    hs::AppSettings&     settings()   { return settings_; }

    void setStatus(const QString& text, bool busy = false);
    void log(const QString& line);

signals:
    void logLine(const QString& line);

public slots:
    void refreshAll();
    void runPreflight();
    void applyFix(const QString& action);
    void startHermes(hs::StartMode mode);
    void stopHermes();
    void saveSettings();
    void startUpdate(hs::ComponentId id);
    void startVersionedUpdate(hs::ComponentId id);  // 需用户选版本 (Node)

    // ---- 便携版构建 (ADR-004) ----
    void resolveBuildVersions();   // 查官方最新 runtime / web-ui 版本
    void startBuild();             // 一键构建
    void cancelBuild();

private slots:
    void onNavChanged(int row);
    // 无参槽: 结果从 QFutureWatcher::result() 取, 避免为 Core 类型注册元类型
    void onPreflightDone();
    void onStartDone(const QString& message, bool ok);
    void onScanDone();
    void onUpdateDone();
    void onBuildResolveDone();
    void onBuildRunDone();

private:
    void buildUi();
    void loadSettings();
    void ensureStudioDirs();
    void updateWindowState();   // 更新窗口标题 + 状态栏 (状态/IP/端口)
    void migrateLegacyStudio();
    void firstRunMigration();
    void buildPluginNav();
    void setupTray();
    void closeEvent(QCloseEvent* e) override;

    QListWidget*    nav_     = nullptr;
    QStackedWidget* stack_   = nullptr;
    QLabel*         status_  = nullptr;

    HomePage*        home_        = nullptr;
    DiagnosticsPage* diagnostics_ = nullptr;
    ProvidersPage*   providers_   = nullptr;
    ModelsPage*      models_      = nullptr;
    UpdatesPage*     updates_     = nullptr;
    PluginsPage*     plugins_     = nullptr;
    SettingsPage*    settingsPage_ = nullptr;
    LogsPage*        logs_         = nullptr;
    BuildPage*       build_        = nullptr;
    QSystemTrayIcon* tray_         = nullptr;
    QMenu*           trayMenu_     = nullptr;
    QLabel*          stateLabel_   = nullptr;   // 状态栏右侧: 状态/IP/端口
    bool             quitting_     = false;   // 退出中, 允许关闭窗口(绕过最小化拦截)

    std::unique_ptr<hs::ModelRegistry>   registry_;
    std::unique_ptr<hs::ConfigRenderer>  renderer_;
    std::unique_ptr<hs::Launcher>        launcher_;
    std::unique_ptr<hs::Updater>         updater_;
    std::unique_ptr<hs::PreflightChecker> checker_;
    std::unique_ptr<hs::PluginHost>      pluginHost_;

    std::unique_ptr<QFutureWatcher<QVector<hs::CheckItem>>>      preflightWatcher_;
    std::unique_ptr<QFutureWatcher<QVector<hs::ComponentState>>> scanWatcher_;
    std::unique_ptr<QFutureWatcher<hs::Updater::ApplyResult>>    updateWatcher_;
    std::unique_ptr<QFutureWatcher<hs::PortableBuilder::RemoteInfo>> buildResolveWatcher_;
    std::unique_ptr<QFutureWatcher<hs::BuildResult>>                 buildWatcher_;
    std::shared_ptr<std::atomic<bool>>                           buildCancel_;

    QVector<hs::CheckItem> lastChecks_;

    hs::AppSettings settings_;
    bool migrated_ = false;
};
