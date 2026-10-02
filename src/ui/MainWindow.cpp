#include "MainWindow.h"
#include "HomePage.h"
#include "DiagnosticsPage.h"
#include "ProvidersPage.h"
#include "ModelsPage.h"
#include "UpdatesPage.h"
#include "PluginsPage.h"
#include "SettingsPage.h"
#include "LogsPage.h"
#include "BuildPage.h"

#include "../app/Paths.h"
#include "../storage/JsonStore.h"

#include <QtConcurrent/QtConcurrentRun>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QStatusBar>
#include <QInputDialog>
#include <QStringList>
#include <QMessageBox>
#include <QDesktopServices>
#include <QUrl>
#include <QDateTime>
#include <QFont>
#include <QAction>
#include <QStyle>
#include <QCloseEvent>
#include <QApplication>
#include <QNetworkInterface>
#include <QHostAddress>
#include <QAbstractSocket>
#include <QTimer>
#include <fstream>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    ensureStudioDirs();
    loadSettings();
    saveSettings();   // 持久化默认设置 (含 hermesRoot), 供插件 config_get 读取

    registry_ = std::make_unique<hs::ModelRegistry>(hs::Paths::studioDir() / "models.json");
    (void)registry_->load();

    // 首次运行: 从旧配置中心的产物迁移
    std::error_code ec;
    if (!fs::exists(hs::Paths::studioDir() / "models.json", ec)) {
        firstRunMigration();
    }

    renderer_ = std::make_unique<hs::ConfigRenderer>(
        hs::Paths::envFile(), hs::Paths::configFile(), hs::Paths::configTemplate());
    launcher_ = std::make_unique<hs::Launcher>();
    updater_  = std::make_unique<hs::Updater>();
    pluginHost_  = std::make_unique<hs::PluginHost>();
    pluginHost_->loadAll();

    preflightWatcher_ = std::make_unique<QFutureWatcher<QVector<hs::CheckItem>>>();
    scanWatcher_      = std::make_unique<QFutureWatcher<QVector<hs::ComponentState>>>();
    updateWatcher_    = std::make_unique<QFutureWatcher<hs::Updater::ApplyResult>>();
    buildResolveWatcher_ = std::make_unique<QFutureWatcher<hs::PortableBuilder::RemoteInfo>>();
    buildWatcher_        = std::make_unique<QFutureWatcher<hs::BuildResult>>();
    buildCancel_         = std::make_shared<std::atomic<bool>>(false);

    connect(preflightWatcher_.get(), &QFutureWatcher<QVector<hs::CheckItem>>::finished,
            this, &MainWindow::onPreflightDone);
    connect(scanWatcher_.get(), &QFutureWatcher<QVector<hs::ComponentState>>::finished,
            this, &MainWindow::onScanDone);
    connect(updateWatcher_.get(), &QFutureWatcher<hs::Updater::ApplyResult>::finished,
            this, &MainWindow::onUpdateDone);
    connect(buildResolveWatcher_.get(), &QFutureWatcher<hs::PortableBuilder::RemoteInfo>::finished,
            this, &MainWindow::onBuildResolveDone);
    connect(buildWatcher_.get(), &QFutureWatcher<hs::BuildResult>::finished,
            this, &MainWindow::onBuildRunDone);

    buildUi();
    setupTray();

    // 页面信号 -> 核心动作
    connect(home_, &HomePage::startRequested, this, &MainWindow::startHermes);
    connect(home_, &HomePage::stopRequested,  this, &MainWindow::stopHermes);
    connect(home_, &HomePage::openBrowserRequested, this, [this] {
        QDesktopServices::openUrl(QUrl(QString::fromStdString(launcher_->webUiUrl(settings_.runtime.webUiPort))));
    });

    connect(diagnostics_, &DiagnosticsPage::recheckRequested, this, &MainWindow::runPreflight);
    connect(diagnostics_, &DiagnosticsPage::fixRequested, this, &MainWindow::applyFix);

    auto onConfigChanged = [this] {
        // 配置变更后立即渲染, 让 Hermes 下次启动就能拿到新配置
        auto r = renderer_->renderAll(registry_->config(), settings_.runtime);
        if (r.ok()) {
            setStatus("配置已保存并渲染到 .env / config.yaml");
            providers_->reload();
            models_->reload();
            runPreflight();
        } else {
            setStatus(QString::fromStdString(r.error->message));
        }
    };
    connect(providers_, &ProvidersPage::configChanged, this, onConfigChanged);
    connect(models_, &ModelsPage::configChanged, this, onConfigChanged);
    // 注意: setStatus 有两个形参 (第二个带默认值), Qt 的 connect 仍按两个算,
    // 直接连 QString 单参信号会触发 static_assert, 所以用 lambda 转发。
    connect(providers_, &ProvidersPage::statusMessage, this, [this](const QString& t) { setStatus(t); });
    connect(models_, &ModelsPage::statusMessage, this, [this](const QString& t) { setStatus(t); });
    connect(plugins_, &PluginsPage::statusMessage, this, [this](const QString& t) { setStatus(t); });
    connect(settingsPage_, &SettingsPage::statusMessage, this, [this](const QString& t) { setStatus(t); });
    connect(settingsPage_, &SettingsPage::settingsSaved, this, &MainWindow::saveSettings);

    connect(updates_, &UpdatesPage::checkRequested, this, [this](bool remote) {
        updates_->setBusy(true);
        scanWatcher_->setFuture(QtConcurrent::run([this, remote] {
            std::vector<hs::ComponentState> v = remote
                ? updater_->scanWithRemote("stable")
                : updater_->scanLocal();
            return QVector<hs::ComponentState>(v.begin(), v.end());
        }));
    });

    connect(updates_, &UpdatesPage::updateRequested, this, &MainWindow::startUpdate);
    connect(updates_, &UpdatesPage::updateVersionRequested, this, &MainWindow::startVersionedUpdate);

    connect(build_, &BuildPage::resolveRequested, this, &MainWindow::resolveBuildVersions);
    connect(build_, &BuildPage::buildRequested,   this, &MainWindow::startBuild);
    connect(build_, &BuildPage::cancelRequested,  this, &MainWindow::cancelBuild);

    refreshAll();
    // 定时刷新窗口状态 (Hermes 状态由端口/pid 轮询, 需周期性更新)
    auto* stateTimer = new QTimer(this);
    stateTimer->setInterval(2000);
    connect(stateTimer, &QTimer::timeout, this, [this] {
        home_->setState(launcher_->state());
        updateWindowState();
    });
    stateTimer->start();
}

void MainWindow::startUpdate(hs::ComponentId id) {
    // 支持一键更新: 内核 / Web UI (都走官方源)。
    // Node 运行时在官方布局下已是纯净发行版, 不再走「桌面端搬运」那条旧路;
    // Python/uv 更新会让 venv 失效, 属高风险, 由 PortableBuilder 重建更安全。
    if (id != hs::ComponentId::Kernel && id != hs::ComponentId::WebUI) {
        setStatus("该组件暂不支持一键更新");
        updates_->setUpdating(false, "该组件暂不支持一键更新");
        return;
    }
    // 先停掉正在运行的 Hermes, 避免文件占用导致替换失败
    bool wasRunning = launcher_->state() == hs::HermesState::Running;
    if (wasRunning) {
        setStatus("更新前正在停止 Hermes...", true);
        launcher_->stop();
    }
    auto target = updater_->checkRemote(id, "stable");
    std::string ver = target.latestVersion;
    if (ver.empty()) { setStatus("无法确定目标版本"); return; }

    updates_->setUpdating(true, "开始更新...");
    auto* raw = updater_.get();
    updateWatcher_->setFuture(QtConcurrent::run([raw, id, ver] {
        if (id == hs::ComponentId::Kernel)
            return raw->applyKernelUpdate(ver);
        return raw->applyWebUiUpdate(ver);
    }));
}

void MainWindow::startVersionedUpdate(hs::ComponentId id) {
    // 目前只有 Node 走「选版本」: 后台拉 nodejs.org 可用版本列表,
    // 前台弹 QInputDialog 让用户挑一个, 然后 applyNodeUpdate。
    if (id != hs::ComponentId::NodeRuntime) { startUpdate(id); return; }

    updates_->setUpdating(true, "获取 Node 可用版本...");
    auto* raw = updater_.get();
    // 用一个一次性 watcher 承接版本列表
    auto* w = new QFutureWatcher<std::vector<std::string>>(this);
    connect(w, &QFutureWatcher<std::vector<std::string>>::finished, this,
            [this, raw, w]() {
        const auto versions = w->result();
        w->deleteLater();
        updates_->setUpdating(false);
        if (versions.empty()) {
            setStatus("获取 Node 版本列表失败 (网络或 nodejs.org 不可达)");
            return;
        }
        QStringList items;
        for (const auto& v : versions) items << QString::fromStdString(v);
        bool ok = false;
        const QString pick = QInputDialog::getItem(this, "选择 Node 版本",
            "可用版本 (从新到旧):", items, 0, false, &ok);
        if (!ok || pick.isEmpty()) { setStatus("已取消"); return; }

        const std::string ver = pick.toStdString();
        // 先停 Hermes 避免占用 node.exe
        bool wasRunning = launcher_->state() == hs::HermesState::Running;
        if (wasRunning) { setStatus("更新前正在停止 Hermes...", true); launcher_->stop(); }

        updates_->setUpdating(true, "开始更新 Node ...");
        updateWatcher_->setFuture(QtConcurrent::run([raw, ver] {
            return raw->applyNodeUpdate(ver);
        }));
    });
    w->setFuture(QtConcurrent::run([raw] { return raw->listNodeVersions(30); }));
}

// ---------------------------------------------------------------------------
// 便携版构建 (ADR-004): 一键从官方 releases 组装完整便携版
// ---------------------------------------------------------------------------
void MainWindow::resolveBuildVersions() {
    if (buildResolveWatcher_->isRunning()) return;
    build_->setBusy(true);
    build_->appendLog("查询官方 releases ...");
    const std::string mirror = build_->mirror().toStdString();
    hs::PortableBuilder builder;
    buildResolveWatcher_->setFuture(QtConcurrent::run([builder, mirror] {
        return builder.resolveLatest("win-x64", mirror);
    }));
}

void MainWindow::onBuildResolveDone() {
    build_->setBusy(false);
    const auto info = buildResolveWatcher_->result();
    if (!info.ok) {
        build_->setResolved({}, {}, QString::fromStdString(info.message));
        build_->appendLog("查询失败: " + QString::fromStdString(info.message));
        setStatus("查询官方版本失败");
        return;
    }
    build_->setResolved(QString::fromStdString(info.runtimeVersion),
                        QString::fromStdString(info.webUiVersion),
                        QString::fromStdString(info.runtimeTag + " / " + info.webUiTag));
    build_->appendLog(QString("Hermes runtime %1  (%2 MB)  sha256 %3")
                          .arg(QString::fromStdString(info.runtimeVersion))
                          .arg(info.runtimeSize / (1024 * 1024))
                          .arg(QString::fromStdString(info.runtimeSha256).left(16) + "..."));
    build_->appendLog(QString("web-ui %1  (%2 MB)  sha256 %3")
                          .arg(QString::fromStdString(info.webUiVersion))
                          .arg(info.webUiSize / (1024 * 1024))
                          .arg(QString::fromStdString(info.webUiSha256).left(16) + "..."));
    setStatus("官方版本已获取");
}

void MainWindow::startBuild() {
    if (buildWatcher_->isRunning()) return;

    hs::BuildOptions opts;
    const QString td = build_->targetDir();
    if (!td.isEmpty()) opts.targetDir = td.toStdString();
    opts.platform     = "win-x64";
    opts.useCache     = true;
    opts.verifySha256 = true;
    opts.githubMirror = build_->mirror().toStdString();

    std::error_code ec;
    const QString shown = td.isEmpty()
        ? QString::fromStdString(hs::Paths::portableRoot().string())
        : td;
    const bool existing = !opts.targetDir.empty() &&
                          fs::exists(opts.targetDir / "python" / "venv", ec);

    // 覆盖构建前先停掉正在跑的服务, 否则 node/python 的 exe 会被占用
    if (launcher_->state() == hs::HermesState::Running) {
        build_->appendLog("检测到 Hermes 正在运行, 先停止 ...");
        launcher_->stop();
        updateWindowState();
    }

    buildCancel_->store(false);
    build_->setBusy(true);
    build_->setProgress(0, "准备中");
    build_->appendLog(QString("开始构建 → %1%2").arg(shown, existing ? "（覆盖已有便携包）" : "（全新建构）"));

    auto cancel    = buildCancel_;
    auto* page     = build_;
    auto lastStage = std::make_shared<std::string>();
    hs::PortableBuilder builder;

    // 进度回调在后台线程触发; 通过 QueuedConnection 回主线程刷新 UI
    buildWatcher_->setFuture(QtConcurrent::run([builder, opts, cancel, page, lastStage] {
        return builder.run(
            opts,
            [page, lastStage](const hs::BuildProgress& p) {
                const std::string stage = p.stage;
                const bool newStage = (stage != *lastStage);
                *lastStage = stage;
                const int pct = p.percent;
                QMetaObject::invokeMethod(page, [page, pct, stage, newStage] {
                    page->setProgress(pct, QString::fromStdString(stage));
                    if (newStage) page->appendLog(QString::fromStdString(stage));
                }, Qt::QueuedConnection);
            },
            [cancel] { return cancel->load(); });
    }));
}

void MainWindow::cancelBuild() {
    if (!buildWatcher_->isRunning()) return;
    buildCancel_->store(true);
    build_->appendLog("已请求取消 —— 下载会在下一个数据块处停下");
    build_->setProgress(0, "正在取消 ...");
}

void MainWindow::onBuildRunDone() {
    const auto res = buildWatcher_->result();
    build_->setBusy(false);

    if (res.ok) {
        build_->setProgress(100, "完成");
        build_->setResult(true, QString::fromStdString(res.message));
        build_->appendLog("目标目录: " + QString::fromStdString(res.targetDir.string()));
        setStatus("便携版构建完成");
        log("便携版构建完成: " + QString::fromStdString(res.targetDir.string()));
        // 若就是当前包, 刷新组件版本
        updates_->setBusy(true);
        scanWatcher_->setFuture(QtConcurrent::run([this] {
            std::vector<hs::ComponentState> v = updater_->scanLocal();
            return QVector<hs::ComponentState>(v.begin(), v.end());
        }));
        return;
    }
    if (res.cancelled) {
        build_->setResult(false, "已取消");
        setStatus("构建已取消");
        return;
    }
    const QString msg = QString::fromStdString(res.message);
    build_->setResult(false, msg);
    setStatus("构建失败");
    log("便携版构建失败: " + msg);
}

MainWindow::~MainWindow() = default;

void MainWindow::ensureStudioDirs() {
    // 旧版本把启动器私有数据放在 data/studio (Hermes 的数据目录)。现迁移到包根
    // studio/ 与 plugin/, 让 data/ 纯粹归 Hermes。只迁移一次。
    migrateLegacyStudio();

    hs::JsonStore::ensureParent(hs::Paths::studioDir() / "settings.json");
    hs::JsonStore::ensureParent(hs::Paths::logsDir() / "x.log");
    hs::JsonStore::ensureParent(hs::Paths::backupDir() / "x");
    hs::JsonStore::ensureParent(hs::Paths::downloadDir() / "x");
}

void MainWindow::migrateLegacyStudio() {
    fs::path legacy = hs::Paths::dataDir() / "studio";   // <root>/data/studio
    fs::path target = hs::Paths::studioDir();            // <root>/studio
    fs::path legacyPlugins = legacy / "plugins";         // <root>/data/studio/plugins
    fs::path pluginTarget  = hs::Paths::pluginDir();     // <root>/plugin

    std::error_code ec;
    // 迁移 helper: 把 src 下每个子项复制/移动到 dst (目标已存在的同名项跳过, 避免覆盖)
    auto moveChildren = [](const fs::path& src, const fs::path& dst) {
        std::error_code ec;
        if (!fs::is_directory(src, ec)) return;
        fs::create_directories(dst, ec);
        for (fs::directory_iterator it(src, ec), end; it != end; ++it) {
            auto name = it->path().filename();
            fs::path to = dst / name;
            if (fs::exists(to, ec)) continue;   // 目标已存在, 跳过
            fs::rename(it->path(), to, ec);
            if (ec) {  // 跨盘 rename 会失败, 退回复制+删除
                ec.clear();
                fs::copy(it->path(), to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
                if (!ec) fs::remove_all(it->path(), ec);
            }
        }
    };

    // 1) 迁移插件: data/studio/plugins/* -> plugin/ (目标已存在的子项跳过)
    if (fs::is_directory(legacyPlugins, ec)) {
        moveChildren(legacyPlugins, pluginTarget);
        // 尝试删除空壳
        fs::remove_all(legacyPlugins, ec);
    }
    // 2) 迁移 data/studio 其余内容 -> studio/
    if (fs::is_directory(legacy, ec)) {
        moveChildren(legacy, target);
        fs::remove_all(legacy, ec);   // 删掉旧目录 (含空壳)
    }
}

void MainWindow::loadSettings() {
    auto j = hs::JsonStore::load(hs::Paths::studioDir() / "settings.json");
    if (j) {
        try {
            settings_ = j->get<hs::AppSettings>();
        } catch (const std::exception&) {
            settings_ = hs::AppSettings{};
        }
    }
    // hermesRoot 存"相对便携包根"的相对路径 (默认 .\Hermes), 可重定位。
    // 若为空或不是相对路径, 从 autoDetect 定位的结果反向推导相对路径。
    if (settings_.runtime.hermesRoot.empty() ||
        !fs::path(settings_.runtime.hermesRoot).is_relative()) {
        fs::path portable = hs::Paths::portableRoot();
        fs::path root = hs::Paths::root();
        if (!portable.empty() && !root.empty()) {
            std::error_code ec;
            auto rel = fs::relative(root, portable, ec);
            if (!rel.empty()) settings_.runtime.hermesRoot = rel.string();
        }
    }
}

void MainWindow::saveSettings() {
    json j = settings_;
    (void)hs::JsonStore::save(hs::Paths::studioDir() / "settings.json", j);
}

void MainWindow::firstRunMigration() {
    auto rep = registry_->migrateFromLegacy(hs::Paths::envFile(), hs::Paths::configFile(), hs::Paths::configTemplate());
    QString msg = QString("已从旧配置迁移: %1 个 Provider, %2 个模型")
                      .arg(rep.providersImported).arg(rep.modelsImported);
    for (const auto& w : rep.warnings) {
        msg += "\n- " + QString::fromStdString(w);
    }
    migrated_ = true;
    QMessageBox::information(this, "首次运行 — 配置迁移", msg);
}

void MainWindow::buildUi() {
    auto* central = new QWidget(this);
    auto* lay = new QHBoxLayout(central);
    lay->setContentsMargins(0, 0, 0, 0);

    // ---- 左侧导航 ----
    nav_ = new QListWidget(this);
    nav_->setFixedWidth(150);
    nav_->setSpacing(2);
    QStringList items = {"概览", "体检", "Provider", "模型", "更新", "构建", "插件", "设置", "日志"};
    for (const auto& s : items) nav_->addItem(s);
    nav_->setCurrentRow(0);
    lay->addWidget(nav_);

    // ---- 右侧页面栈 ----
    stack_ = new QStackedWidget(this);
    home_        = new HomePage(this);
    diagnostics_ = new DiagnosticsPage(this);
    providers_   = new ProvidersPage(this);
    models_      = new ModelsPage(this);
    updates_     = new UpdatesPage(this);
    plugins_     = new PluginsPage(this);
    settingsPage_ = new SettingsPage(this);
    logs_         = new LogsPage(this);
    build_        = new BuildPage(this);

    providers_->setRegistry(registry_.get());
    models_->setRegistry(registry_.get());
    plugins_->setHost(pluginHost_.get());
    settingsPage_->setSettings(&settings_);

    stack_->addWidget(home_);
    stack_->addWidget(diagnostics_);
    stack_->addWidget(providers_);
    stack_->addWidget(models_);
    stack_->addWidget(updates_);
    stack_->addWidget(build_);
    stack_->addWidget(plugins_);
    stack_->addWidget(settingsPage_);
    stack_->addWidget(logs_);

    // 插件 UI 嵌入: 每个启用插件声明的 UI 入口在导航追加一项, 右侧显示对应面板
    buildPluginNav();

    lay->addWidget(stack_, 1);

    connect(nav_, &QListWidget::currentRowChanged, this, &MainWindow::onNavChanged);

    setCentralWidget(central);

    status_ = new QLabel("就绪");
    statusBar()->addWidget(status_, 1);
    stateLabel_ = new QLabel("状态: 未启动", this);
    stateLabel_->setStyleSheet("color: #666; padding-right: 8px;");
    statusBar()->addPermanentWidget(stateLabel_);

    resize(980, 660);
    updateWindowState();   // 初始标题 + 状态栏
}

void MainWindow::onNavChanged(int row) {
    if (row >= 0 && row < stack_->count()) stack_->setCurrentIndex(row);
}

void MainWindow::setupTray() {
    tray_ = new QSystemTrayIcon(this);
    tray_->setIcon(QApplication::style()->standardIcon(QStyle::SP_ComputerIcon));
    tray_->setToolTip("AgentDock — 多 Agent 便携启动器");

    trayMenu_ = new QMenu(this);
    QAction* showAct = trayMenu_->addAction("显示 / 隐藏窗口");
    QAction* startAct = trayMenu_->addAction("启动 Hermes (WebUI)");
    QAction* stopAct  = trayMenu_->addAction("停止 Hermes");
    trayMenu_->addSeparator();
    QAction* quitAct  = trayMenu_->addAction("退出");
    tray_->setContextMenu(trayMenu_);

    connect(showAct, &QAction::triggered, this, [this] {
        if (isVisible()) { hide(); }
        else { show(); raise(); activateWindow(); }
    });
    connect(startAct, &QAction::triggered, this, [this] { startHermes(hs::StartMode::WebUI); });
    connect(stopAct,  &QAction::triggered, this, [this] { stopHermes(); });
    connect(quitAct,  &QAction::triggered, this, [this] {
        // 置退出标志, 让 closeEvent 放行(不弹最小化提示、不拦截)
        quitting_ = true;
        if (launcher_->state() == hs::HermesState::Running) {
            if (QMessageBox::question(this, "确认退出",
                "Hermes 正在运行, 退出前是否停止?") == QMessageBox::Yes) {
                launcher_->stop();
            }
        }
        qApp->quit();
    });
    connect(tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
            if (isVisible()) hide(); else { show(); raise(); activateWindow(); }
        }
    });

    tray_->show();
    // 关闭时最小化到托盘而非退出
    connect(qApp, &QApplication::aboutToQuit, this, [this] {
        if (launcher_->state() == hs::HermesState::Running) launcher_->stop();
    });
}

void MainWindow::closeEvent(QCloseEvent* e) {
    // 托盘菜单"退出"触发: 直接放行, 不弹最小化提示
    if (quitting_) { e->accept(); return; }
    // 点关闭按钮 = 最小化到托盘, 不退出
    if (tray_ && tray_->isVisible() && QSystemTrayIcon::isSystemTrayAvailable()) {
        hide();
        tray_->showMessage("AgentDock", "已最小化到系统托盘, 可随时打开。", QSystemTrayIcon::Information, 1500);
        e->ignore();
    } else {
        e->accept();
    }
}

void MainWindow::buildPluginNav() {
    if (!pluginHost_) return;
    // 每个启用插件声明的 UI 入口, 追加为导航项 + 右侧面板页。
    // 面板展示插件信息 + 当前配置 + 插件声明的动作按钮 (点击调 executeAction)。
    for (auto& p : pluginHost_->loadedPlugins()) {
        for (auto& entry : p.uiEntries) {
            // 追加导航项
            QString navLabel = "· " + QString::fromStdString(entry.title);
            nav_->addItem(navLabel);

            // 创建面板页
            auto* panel = new QWidget(this);
            auto* pl = new QVBoxLayout(panel);
            auto* title = new QLabel(QString::fromStdString(entry.title), panel);
            QFont f = title->font(); f.setPointSize(14); f.setBold(true); title->setFont(f);
            pl->addWidget(title);

            auto* info = new QLabel(
                QString("插件: %1 (%2) · 入口: %3")
                    .arg(QString::fromStdString(p.name),
                         QString::fromStdString(p.version),
                         QString::fromStdString(entry.id)),
                panel);
            info->setWordWrap(true);
            info->setStyleSheet("color: #555;");
            pl->addWidget(info);

            // 动作按钮 (从 entry.actions JSON 解析)
            if (!entry.actions.empty()) {
                try {
                    auto acts = nlohmann::json::parse(entry.actions);
                    if (acts.is_array()) {
                        for (auto& a : acts) {
                            if (!a.is_object()) continue;
                            std::string aid = a.value("id", "");
                            std::string label = a.value("label", aid);
                            std::string confirm = a.value("confirm", "");
                            auto* btn = new QPushButton(QString::fromStdString(label), panel);
                            // 捕获 pluginId, entryId, actionId 用于回调
                            auto pluginId = p.id;
                            pl->addWidget(btn);
                            connect(btn, &QPushButton::clicked, this, [this, pluginId, aid, confirm] {
                                if (!confirm.empty()) {
                                    if (QMessageBox::question(this, "确认操作",
                                        QString::fromStdString(confirm)) != QMessageBox::Yes) return;
                                }
                                hs_i32 rc = pluginHost_->executeAction(pluginId, aid, "");
                                setStatus(rc == 0 ? ("插件动作完成: " + QString::fromStdString(aid))
                                                  : ("插件动作失败 (code=" + QString::number(rc) + ")"));
                            });
                        }
                    }
                } catch (const std::exception&) {
                    // actions JSON 解析失败, 忽略
                }
            }

            // 展示当前配置
            auto cfg = hs::JsonStore::load(hs::Paths::studioDir() / "app.json");
            QString cfgText = "(暂无配置)";
            if (cfg && cfg->contains(entry.id)) {
                cfgText = QString::fromStdString((*cfg)[entry.id].dump(2));
            }
            auto* cfgLabel = new QLabel("当前配置:\n" + cfgText, panel);
            cfgLabel->setWordWrap(true);
            cfgLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
            cfgLabel->setStyleSheet("font-family: Consolas; color: #333; background:#f5f5f5; padding:6px;");
            pl->addWidget(cfgLabel);
            pl->addStretch();

            stack_->addWidget(panel);
        }
    }
}

void MainWindow::setStatus(const QString& text, bool busy) {
    status_->setText((busy ? "[执行中] " : "") + text);
    log(text);
}

void MainWindow::log(const QString& line) {
    QString stamped = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss") + "  " + line;
    home_->appendLog(stamped);
    if (logs_) logs_->appendLine(stamped);
    // 追加写入日志文件
    try {
        std::error_code ec;
        fs::create_directories(hs::Paths::logsDir(), ec);
        std::ofstream out(hs::Paths::logsDir() / "launcher.log", std::ios::app);
        if (out) out << stamped.toStdString() << "\n";
    } catch (...) {}
}

// 获取本机 LAN IP (首个非回环的 IPv4 地址)
static QString localIp() {
    const auto ifaces = QNetworkInterface::allInterfaces();
    for (const auto& iface : ifaces) {
        if (!(iface.flags() & QNetworkInterface::IsUp)) continue;
        if (iface.flags() & QNetworkInterface::IsLoopBack) continue;
        const auto entries = iface.addressEntries();
        for (const auto& e : entries) {
            auto ip = e.ip();
            if (ip.protocol() == QAbstractSocket::IPv4Protocol && !ip.isLoopback()) {
                return ip.toString();
            }
        }
    }
    return QString();
}

void MainWindow::updateWindowState() {
    auto st = launcher_->state();
    QString stateText;
    switch (st) {
        case hs::HermesState::Stopped:  stateText = "未启动"; break;
        case hs::HermesState::Starting: stateText = "启动中"; break;
        case hs::HermesState::Running:  stateText = "运行中"; break;
        case hs::HermesState::Stopping: stateText = "停止中"; break;
        case hs::HermesState::Failed:   stateText = "异常"; break;
        default: stateText = "未知"; break;
    }
    // 窗口标题: AgentDock vX — Hermes [运行中]
    setWindowTitle(QString("AgentDock %1 — Hermes %2").arg(HERMESSTUDIO_VERSION, stateText));

    // 状态栏右侧: 状态 · IP · 端口
    int port = settings_.runtime.webUiPort;
    QString right = "状态: " + stateText;
    if (st == hs::HermesState::Running) {
        QString ip = localIp();
        right += " · IP: " + (ip.isEmpty() ? "-" : ip);
        right += " · 端口: " + QString::number(port);
        right += " · " + QString::fromStdString(launcher_->webUiUrl(port));
    }
    if (stateLabel_) stateLabel_->setText(right);
}

void MainWindow::refreshAll() {
    home_->setRootPath(QString::fromStdString(hs::Paths::root().string()));
    auto def = registry_->defaultModel();
    home_->setDefaultModel(def ? QString::fromStdString(def->displayName) : QString());

    home_->setState(launcher_->state());

    for (auto m : {hs::StartMode::Cli, hs::StartMode::WebUI, hs::StartMode::Desktop}) {
        home_->setModeAvailable(m, launcher_->modeAvailable(m),
                                QString::fromStdString(launcher_->modeUnavailableReason(m)));
    }

    providers_->reload();
    models_->reload();
    plugins_->reload();
    settingsPage_->reload();
    runPreflight();

    updates_->setBusy(true);
    scanWatcher_->setFuture(QtConcurrent::run([this] {
        auto v = updater_->scanLocal();
        return QVector<hs::ComponentState>(v.begin(), v.end());
    }));
}

void MainWindow::runPreflight() {
    hs::PreflightChecker::Context ctx;
    ctx.modelConfig = registry_->config();
    ctx.runtime     = settings_.runtime;
    std::error_code ec;
    ctx.haveModels  = fs::exists(hs::Paths::studioDir() / "models.json", ec);
    ctx.hermesRunning = launcher_->state() == hs::HermesState::Running;

    checker_ = std::make_unique<hs::PreflightChecker>(ctx);
    // 捕获裸指针: PreflightChecker 生命周期由 unique_ptr 管理, future 在窗口存活期内完成
    auto* raw = checker_.get();
    preflightWatcher_->setFuture(QtConcurrent::run([raw] {
        auto v = raw->runAll();
        return QVector<hs::CheckItem>(v.begin(), v.end());
    }));
    setStatus("正在体检...", true);
}

void MainWindow::onPreflightDone() {
    lastChecks_ = preflightWatcher_->result();
    diagnostics_->setItems(lastChecks_);

    bool blocked = false;
    QString reason;
    for (const auto& c : lastChecks_) {
        if (c.level == hs::CheckLevel::Fail) {
            blocked = true;
            reason = QString::fromStdString(c.title);
            break;
        }
    }
    home_->setLaunchBlocked(blocked, reason);
    setStatus(blocked ? ("体检发现阻断项: " + reason) : "体检通过");
}

void MainWindow::applyFix(const QString& action) {
    if (!checker_) return;
    std::string a = action.toStdString();
    setStatus("正在修复: " + action, true);
    auto* raw = checker_.get();
    auto future = QtConcurrent::run([raw, a] { return raw->applyFix(a); });
    // 修复多为秒级, 同步等待后刷新
    QString result = QString::fromStdString(future.result());
    log("修复结果: " + result);
    runPreflight();
}

void MainWindow::startHermes(hs::StartMode mode) {
    if (!launcher_->modeAvailable(mode)) {
        setStatus(QString::fromStdString(launcher_->modeUnavailableReason(mode)));
        return;
    }

    // 插件可 veto 启动
    if (!pluginHost_->dispatch(HS_EVENT_APP_STARTING)) {
        setStatus("插件阻止了启动");
        return;
    }

    // 渲染配置 -> 启动
    auto r = renderer_->renderAll(registry_->config(), settings_.runtime);
    if (!r.ok()) {
        onStartDone(QString::fromStdString(r.error->message), false);
        return;
    }
    log("已渲染 .env / config.yaml");

    setStatus("正在启动...", true);
    auto* raw = launcher_.get();
    int port = settings_.runtime.webUiPort;
    auto future = QtConcurrent::run([raw, mode, port] { return raw->start(mode, port); });
    // 启动是异步的 (进程拉起即返回), 同步取结果不会长时间阻塞
    auto res = future.result();
    onStartDone(QString::fromStdString(res.message), res.ok);
}

void MainWindow::onStartDone(const QString& message, bool ok) {
    log(message);
    setStatus(message);
    if (!ok) {
        QMessageBox::warning(this, "启动失败", message);
    }
    home_->setState(launcher_->state());
    updateWindowState();
}

void MainWindow::stopHermes() {
    setStatus("正在停止...", true);
    bool ok = launcher_->stop();
    QString msg = ok ? "已停止 Hermes 相关进程并清理锁文件" : "部分进程未能停止, 请手动检查";
    log(msg);
    setStatus(msg);
    home_->setState(launcher_->state());
    updateWindowState();
}

void MainWindow::onScanDone() {
    auto states = scanWatcher_->result();
    updates_->setComponents(states);
    updates_->setBusy(false);

    for (const auto& s : states) {
        if (s.id == hs::ComponentId::Kernel) {
            home_->setKernelVersion(QString::fromStdString(s.currentVersion));
        } else if (s.id == hs::ComponentId::WebUI) {
            home_->setWebUiVersion(QString::fromStdString(s.currentVersion));
        }
    }
    setStatus("版本探测完成");
}

void MainWindow::onUpdateDone() {
    auto res = updateWatcher_->result();
    updates_->setUpdating(false, QString::fromStdString(res.message));
    if (res.ok) {
        setStatus(QString::fromStdString(res.message));
    } else {
        setStatus(QString::fromStdString(res.message));
        QMessageBox::warning(this, "更新失败", QString::fromStdString(res.message));
    }
    // 更新后重新探测本地 + 远端版本 (保留最新版本列, 避免远端信息丢失)
    updates_->setBusy(true);
    scanWatcher_->setFuture(QtConcurrent::run([this] {
        auto v = updater_->scanWithRemote("stable");
        return QVector<hs::ComponentState>(v.begin(), v.end());
    }));
}
