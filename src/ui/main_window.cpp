#include "rrcc/main_window.hpp"

#include <QBrush>
#include <QColor>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonValue>
#include <QFile>
#include <QMenu>
#include <QMessageBox>
#include <QSet>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QTableWidgetItem>
#include <QToolButton>
#include <QDateTime>
#include <QElapsedTimer>
#include <QCryptographicHash>
#include <QVBoxLayout>

#include "rrcc/telemetry.hpp"

namespace rrcc {

namespace {

QString jsonToPretty(const QJsonObject& object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Indented));
}

QString joinArrayLines(const QJsonArray& array) {
    QStringList lines;
    for (const QJsonValue& value : array) {
        if (value.isObject()) {
            lines.append(
                QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact)));
        } else {
            lines.append(value.toVariant().toString());
        }
    }
    return lines.join('\n');
}

qint64 pidFromRow(const QTableWidget* table, int row) {
    if (row < 0 || table->item(row, 0) == nullptr) {
        return -1;
    }
    return table->item(row, 0)->text().toLongLong();
}

QString hashArray(const QJsonArray& value) {
    const QByteArray payload = QJsonDocument(value).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha1).toHex());
}

}  // namespace

MainWindow::MainWindow() {
    setupUi();
    setupWorker();
    setupConnections();
    applyMode();
    queueRefresh();
}

MainWindow::~MainWindow() {
    if (workerThread_ != nullptr) {
        workerThread_->quit();
        workerThread_->wait(3000);
    }
}

void MainWindow::setupUi() {
    setWindowTitle("Roscoppe");
    resize(1560, 940);
    setMinimumSize(1100, 700);
    setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);

    central_ = new QWidget(this);
    setCentralWidget(central_);

    auto* root = new QVBoxLayout(central_);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    auto* header = new QHBoxLayout();
    auto* title = new QLabel("Roscoppe");
    title->setStyleSheet("font-size: 20px; font-weight: 700;");
    auto* aboutButton = new QPushButton("About");

    modeCombo_ = new QComboBox();
    modeCombo_->addItems({"Engineer", "Operator"});

    auto* refreshButton = new QPushButton("Refresh");
    emergencyStopButton_ = new QPushButton("Emergency Stop (Kill ROS)");
    emergencyStopButton_->setStyleSheet("background:#d64545;color:white;font-weight:700;");
    snapshotJsonButton_ = new QPushButton("Snapshot JSON");
    snapshotYamlButton_ = new QPushButton("Snapshot YAML");
    compareSnapshotButton_ = new QPushButton("Snapshot Diff");
    savePresetButton_ = new QPushButton("Save Preset");
    loadPresetButton_ = new QPushButton("Load Preset");
    sessionStartButton_ = new QPushButton("Start Session");
    sessionStopButton_ = new QPushButton("Stop Session");
    sessionExportButton_ = new QPushButton("Export Session");
    telemetryExportButton_ = new QPushButton("Export Telemetry");
    watchdogToggleButton_ = new QPushButton("Watchdog: OFF");
    fleetRefreshButton_ = new QPushButton("Fleet Refresh");
    fleetLoadTargetsButton_ = new QPushButton("Load Fleet Targets");
    remoteRestartButton_ = new QPushButton("Remote Restart");
    remoteKillButton_ = new QPushButton("Remote Kill");
    healthLabel_ = new QLabel("Health: UNKNOWN");
    healthLabel_->setStyleSheet(
        "font-size:16px;font-weight:800;padding:6px 10px;border-radius:8px;background:#eeeeee;color:#333333;");
    presetLabel_ = new QLabel("Preset: default");
    emergencyStopButton_->setMinimumHeight(34);

    auto* snapshotMenu = new QMenu(this);
    snapshotMenu->addAction("JSON", [this]() { snapshotJsonButton_->click(); });
    snapshotMenu->addAction("YAML", [this]() { snapshotYamlButton_->click(); });
    snapshotMenu->addAction("Diff", [this]() { compareSnapshotButton_->click(); });
    auto* snapshotMenuButton = new QToolButton(this);
    snapshotMenuButton->setText("Snapshot");
    snapshotMenuButton->setMenu(snapshotMenu);
    snapshotMenuButton->setPopupMode(QToolButton::InstantPopup);

    auto* sessionMenu = new QMenu(this);
    sessionMenu->addAction("Start", [this]() { sessionStartButton_->click(); });
    sessionMenu->addAction("Stop", [this]() { sessionStopButton_->click(); });
    sessionMenu->addAction("Export", [this]() { sessionExportButton_->click(); });
    sessionMenu->addAction("Export Telemetry", [this]() { telemetryExportButton_->click(); });
    auto* sessionMenuButton = new QToolButton(this);
    sessionMenuButton->setText("Session");
    sessionMenuButton->setMenu(sessionMenu);
    sessionMenuButton->setPopupMode(QToolButton::InstantPopup);

    auto* presetMenu = new QMenu(this);
    presetMenu->addAction("Save", [this]() { savePresetButton_->click(); });
    presetMenu->addAction("Load", [this]() { loadPresetButton_->click(); });
    auto* presetMenuButton = new QToolButton(this);
    presetMenuButton->setText("Preset");
    presetMenuButton->setMenu(presetMenu);
    presetMenuButton->setPopupMode(QToolButton::InstantPopup);

    auto* fleetMenu = new QMenu(this);
    fleetMenu->addAction("Load Targets", [this]() { fleetLoadTargetsButton_->click(); });
    fleetMenu->addAction("Refresh", [this]() { fleetRefreshButton_->click(); });
    fleetMenu->addAction("Remote Restart", [this]() { remoteRestartButton_->click(); });
    fleetMenu->addAction("Remote Kill", [this]() { remoteKillButton_->click(); });
    auto* fleetMenuButton = new QToolButton(this);
    fleetMenuButton->setText("Fleet");
    fleetMenuButton->setMenu(fleetMenu);
    fleetMenuButton->setPopupMode(QToolButton::InstantPopup);

    auto* leftZone = new QHBoxLayout();
    leftZone->addWidget(title);
    leftZone->addWidget(new QLabel("Mode"));
    leftZone->addWidget(modeCombo_);
    leftZone->addWidget(refreshButton);
    leftZone->addWidget(aboutButton);

    auto* centerZone = new QHBoxLayout();
    centerZone->addWidget(snapshotMenuButton);
    centerZone->addWidget(sessionMenuButton);
    centerZone->addWidget(presetMenuButton);
    centerZone->addWidget(fleetMenuButton);
    centerZone->addWidget(presetLabel_);

    auto* rightZone = new QHBoxLayout();
    rightZone->addWidget(healthLabel_);
    rightZone->addWidget(watchdogToggleButton_);
    rightZone->addWidget(emergencyStopButton_);

    header->addLayout(leftZone);
    header->addStretch(1);
    header->addLayout(centerZone);
    header->addStretch(1);
    header->addLayout(rightZone);
    root->addLayout(header);

    tabs_ = new QTabWidget();
    root->addWidget(tabs_, 1);

    auto* processTab = new QWidget();
    auto* processLayout = new QVBoxLayout(processTab);
    auto* processControls = new QHBoxLayout();
    processSearch_ = new QLineEdit();
    processSearch_->setPlaceholderText("Search by PID, name, executable, or command");
    processScopeCombo_ = new QComboBox();
    processScopeCombo_->addItems({"All Processes", "ROS Only", "Domain 0", "Domain 1"});
    processScopeCombo_->setCurrentIndex(1);
    processViewModeCombo_ = new QComboBox();
    processViewModeCombo_->addItems({"Compact View", "Advanced View"});
    processViewModeCombo_->setCurrentIndex(0);
    processPrevButton_ = new QPushButton("Prev");
    processNextButton_ = new QPushButton("Next");
    processPageLabel_ = new QLabel("Rows 0-0 / 0");
    terminateButton_ = new QPushButton("SIGTERM");
    forceKillButton_ = new QPushButton("SIGKILL");
    killTreeButton_ = new QPushButton("Kill Tree");
    processControls->addWidget(processSearch_, 1);
    processControls->addWidget(processScopeCombo_);
    processControls->addWidget(processViewModeCombo_);
    processControls->addWidget(processPrevButton_);
    processControls->addWidget(processNextButton_);
    processControls->addWidget(processPageLabel_);
    processControls->addWidget(terminateButton_);
    processControls->addWidget(forceKillButton_);
    processControls->addWidget(killTreeButton_);
    processLayout->addLayout(processControls);

    processTable_ = new QTableWidget();
    processTable_->setColumnCount(12);
    processTable_->setHorizontalHeaderLabels(
        {"PID", "PPID", "Name", "CPU %", "Mem %", "Threads", "Uptime", "Domain", "Node",
         "Executable", "Workspace", "Launch"});
    processTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    processTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    processTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    processTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    processTable_->horizontalHeader()->setStretchLastSection(true);
    processLayout->addWidget(processTable_, 1);
    applyProcessTableMode();
    tabs_->addTab(processTab, "Processes");

    auto* domainTab = new QWidget();
    auto* domainLayout = new QVBoxLayout(domainTab);
    domainTable_ = new QTableWidget();
    domainTable_->setColumnCount(7);
    domainTable_->setHorizontalHeaderLabels(
        {"Domain", "ROS Processes", "CPU %", "Mem %", "Nodes", "Conflict", "TF2/SLAM"});
    domainTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    domainTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    domainTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    domainTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    domainTable_->horizontalHeader()->setStretchLastSection(true);
    domainLayout->addWidget(domainTable_, 0);

    domainNodeTable_ = new QTableWidget();
    domainNodeTable_->setColumnCount(7);
    domainNodeTable_->setHorizontalHeaderLabels(
        {"Node", "Namespace", "PID", "Executable", "Package", "Workspace", "Launch"});
    domainNodeTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    domainNodeTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    domainNodeTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    domainNodeTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    domainNodeTable_->horizontalHeader()->setStretchLastSection(true);
    domainLayout->addWidget(domainNodeTable_, 1);

    auto* domainControls = new QHBoxLayout();
    restartDomainButton_ = new QPushButton("Restart Domain");
    isolateDomainButton_ = new QPushButton("Isolate Domain");
    clearShmButton_ = new QPushButton("Clear Shared Memory");
    workspacePathInput_ = new QLineEdit();
    workspacePathInput_->setPlaceholderText("Workspace path (e.g. /home/user/ws/install)");
    workspaceRelaunchInput_ = new QLineEdit();
    workspaceRelaunchInput_->setPlaceholderText("Optional relaunch command");
    restartWorkspaceButton_ = new QPushButton("Restart Workspace");
    domainControls->addWidget(restartDomainButton_);
    domainControls->addWidget(isolateDomainButton_);
    domainControls->addWidget(clearShmButton_);
    domainControls->addWidget(workspacePathInput_, 1);
    domainControls->addWidget(workspaceRelaunchInput_, 1);
    domainControls->addWidget(restartWorkspaceButton_);
    domainLayout->addLayout(domainControls);
    tabs_->addTab(domainTab, "ROS Domains");

    auto* nodesTab = new QWidget();
    auto* nodesLayout = new QVBoxLayout(nodesTab);
    auto* nodeSplitter = new QSplitter(Qt::Horizontal);

    nodesTree_ = new QTreeWidget();
    nodesTree_->setColumnCount(4);
    nodesTree_->setHeaderLabels({"Node", "Category", "Name", "Type/QoS"});
    nodeSplitter->addWidget(nodesTree_);

    auto* nodeRight = new QWidget();
    auto* nodeRightLayout = new QVBoxLayout(nodeRight);
    qosText_ = new QPlainTextEdit();
    qosText_->setReadOnly(true);
    qosText_->setPlaceholderText("QoS details and graph alerts...");
    paramsText_ = new QPlainTextEdit();
    paramsText_->setReadOnly(true);
    paramsText_->setPlaceholderText("Selected node parameters...");
    fetchParamsButton_ = new QPushButton("Fetch Parameters for Selected Node");
    nodeRightLayout->addWidget(new QLabel("QoS / Graph Alerts"));
    nodeRightLayout->addWidget(qosText_, 1);
    nodeRightLayout->addWidget(fetchParamsButton_);
    nodeRightLayout->addWidget(new QLabel("Parameters"));
    nodeRightLayout->addWidget(paramsText_, 1);
    nodeSplitter->addWidget(nodeRight);
    nodesLayout->addWidget(nodeSplitter, 1);
    tabs_->addTab(nodesTab, "Nodes & Topics");

    auto* tfTab = new QWidget();
    auto* tfLayout = new QVBoxLayout(tfTab);
    tfTable_ = new QTableWidget();
    tfTable_->setColumnCount(2);
    tfTable_->setHorizontalHeaderLabels({"Parent Frame", "Child Frame"});
    tfTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tfTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    tfTable_->horizontalHeader()->setStretchLastSection(true);
    nav2Text_ = new QPlainTextEdit();
    nav2Text_->setReadOnly(true);
    nav2Text_->setPlaceholderText("TF tree");
    tfLayout->addWidget(tfTable_, 2);
    tfLayout->addWidget(nav2Text_, 1);
    tabs_->addTab(tfTab, "TF");

    auto* systemTab = new QWidget();
    auto* systemLayout = new QVBoxLayout(systemTab);
    auto* summary = new QHBoxLayout();
    cpuLabel_ = new QLabel("CPU: -");
    memLabel_ = new QLabel("Memory: -");
    diskLabel_ = new QLabel("Disk: -");
    gpuLabel_ = new QLabel("GPU: -");
    summary->addWidget(cpuLabel_);
    summary->addWidget(memLabel_);
    summary->addWidget(diskLabel_);
    summary->addWidget(gpuLabel_);
    summary->addStretch(1);
    systemLayout->addLayout(summary);

    auto* hardwareSplitter = new QSplitter(Qt::Horizontal);
    usbText_ = new QPlainTextEdit();
    usbText_->setReadOnly(true);
    usbText_->setPlaceholderText("USB devices");
    serialText_ = new QPlainTextEdit();
    serialText_->setReadOnly(true);
    serialText_->setPlaceholderText("Serial ports");
    canText_ = new QPlainTextEdit();
    canText_->setReadOnly(true);
    canText_->setPlaceholderText("CAN interfaces");
    netText_ = new QPlainTextEdit();
    netText_->setReadOnly(true);
    netText_->setPlaceholderText("Network interfaces");
    hardwareSplitter->addWidget(usbText_);
    hardwareSplitter->addWidget(serialText_);
    hardwareSplitter->addWidget(canText_);
    hardwareSplitter->addWidget(netText_);
    systemLayout->addWidget(hardwareSplitter, 1);
    tabs_->addTab(systemTab, "System & Hardware");

    auto* logsTab = new QWidget();
    auto* logsLayout = new QVBoxLayout(logsTab);
    logsText_ = new QPlainTextEdit();
    logsText_->setReadOnly(true);
    logsLayout->addWidget(logsText_, 1);
    tabs_->addTab(logsTab, "Logs");

    auto* diagnosticsTab = new QWidget();
    auto* diagnosticsLayout = new QVBoxLayout(diagnosticsTab);
    diagnosticsText_ = new QPlainTextEdit();
    diagnosticsText_->setReadOnly(true);
    diagnosticsLayout->addWidget(diagnosticsText_, 1);
    tabs_->addTab(diagnosticsTab, "Diagnostics");

    auto* performanceTab = new QWidget();
    auto* performanceLayout = new QVBoxLayout(performanceTab);
    performanceText_ = new QPlainTextEdit();
    performanceText_->setReadOnly(true);
    performanceLayout->addWidget(performanceText_, 1);
    tabs_->addTab(performanceTab, "Performance");

    auto* safetyTab = new QWidget();
    auto* safetyLayout = new QVBoxLayout(safetyTab);
    safetyText_ = new QPlainTextEdit();
    safetyText_->setReadOnly(true);
    safetyLayout->addWidget(safetyText_, 1);
    tabs_->addTab(safetyTab, "Safety");

    auto* workspaceTab = new QWidget();
    auto* workspaceLayout = new QVBoxLayout(workspaceTab);
    workspaceText_ = new QPlainTextEdit();
    workspaceText_->setReadOnly(true);
    workspaceLayout->addWidget(workspaceText_, 1);
    tabs_->addTab(workspaceTab, "Workspaces");

    auto* fleetTab = new QWidget();
    auto* fleetLayout = new QVBoxLayout(fleetTab);
    fleetText_ = new QPlainTextEdit();
    fleetText_->setReadOnly(true);
    fleetLayout->addWidget(fleetText_, 1);
    tabs_->addTab(fleetTab, "Fleet");
    tabs_->setCurrentIndex(2);

    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(refreshIntervalMs_);
    refreshTimer_->setSingleShot(true);
    refreshDebounceTimer_ = new QTimer(this);
    refreshDebounceTimer_->setInterval(450);
    refreshDebounceTimer_->setSingleShot(true);
    eventLoopLagTimer_ = new QTimer(this);
    eventLoopLagTimer_->setInterval(1000);
    memoryWatchTimer_ = new QTimer(this);
    memoryWatchTimer_->setInterval(5000);
    statusBar()->showMessage("Ready");

    connect(refreshButton, &QPushButton::clicked, this, [this]() { scheduleRefresh(0, true); });
    connect(aboutButton, &QPushButton::clicked, this, [this]() {
        QMessageBox::about(
            this,
            "About Roscoppe",
            "Roscoppe\n\n"
            "A ROS 2 runtime inspector for nodes, domains, TF/Nav2 health, "
            "process diagnostics, and operational controls.\n\n"
            "Built and maintained by Prabal Khare.");
    });
}

void MainWindow::setupWorker() {
    workerThread_ = new QThread(this);
    worker_ = new RuntimeWorker();
    worker_->moveToThread(workerThread_);
    connect(workerThread_, &QThread::finished, worker_, &QObject::deleteLater);
    workerThread_->start();
}

void MainWindow::setupConnections() {
    connect(this, &MainWindow::pollRequested, worker_, &RuntimeWorker::poll, Qt::QueuedConnection);
    connect(
        this,
        &MainWindow::actionRequested,
        worker_,
        &RuntimeWorker::runAction,
        Qt::QueuedConnection);
    connect(
        this,
        &MainWindow::nodeParametersRequested,
        worker_,
        &RuntimeWorker::fetchNodeParameters,
        Qt::QueuedConnection);

    connect(worker_, &RuntimeWorker::snapshotReady, this, [this](const QJsonObject& snapshot) {
        refreshInFlight_ = false;
        renderFromSnapshot(snapshot);
        scheduleRefresh(refreshIntervalMs_);
    });
    connect(worker_, &RuntimeWorker::actionFinished, this, [this](const QJsonObject& result) {
        refreshInFlight_ = false;
        const bool success = result.value("success").toBool(false);
        const QString action = result.value("action").toString();
        QString message = result.value("message").toString();
        if (success && result.contains("path")) {
            message = QString("Snapshot saved: %1").arg(result.value("path").toString());
        }
        if (!success && result.contains("error")) {
            message = result.value("error").toString();
        }
        if (message.isEmpty()) {
            if (success) {
                message = QString("Action %1 completed").arg(action);
            } else {
                message = QString("Action %1 failed").arg(action);
            }
        }

        if ((action == "compare_snapshots" || action == "compare_with_previous") && diagnosticsText_ != nullptr) {
            diagnosticsText_->setPlainText(jsonToPretty(result));
        }
        if ((action == "load_preset" || action == "save_preset") && success && presetLabel_ != nullptr) {
            presetLabel_->setText(
                QString("Preset: %1").arg(result.value("preset_name").toString("default")));
        }
        if ((action == "fleet_refresh" || action == "remote_action") && result.contains("fleet")) {
            cachedFleet_ = result.value("fleet").toObject();
            if (fleetText_ != nullptr) {
                fleetText_->setPlainText(jsonToPretty(cachedFleet_));
            }
        }
        if ((action == "watchdog_enable" || action == "watchdog_disable") && watchdogToggleButton_ != nullptr) {
            const bool enabled = action == "watchdog_enable";
            watchdogToggleButton_->setText(enabled ? "Watchdog: ON" : "Watchdog: OFF");
        }

        showMessage(message, !success);
        scheduleRefresh(300, true);
    });
    connect(worker_, &RuntimeWorker::nodeParametersReady, this, [this](const QJsonObject& result) {
        const QString node = result.value("node").toString();
        if (result.value("success").toBool(false)) {
            const QString parameters = result.value("parameters").toString();
            cachedNodeParameters_.insert(node, parameters);
            nodeParameterOrder_.append(node);
            nodeParameterOrder_.removeDuplicates();
            pruneNodeParameterCache();
            paramsText_->setPlainText(parameters);
            showMessage(QString("Loaded parameters for %1").arg(node));
        } else {
            paramsText_->setPlainText(result.value("error").toString());
            showMessage(QString("Failed to load parameters for %1").arg(node), true);
        }
    });

    connect(refreshTimer_, &QTimer::timeout, this, [this]() { queueRefresh(); });
    connect(refreshDebounceTimer_, &QTimer::timeout, this, [this]() { queueRefresh(); });
    connect(eventLoopLagTimer_, &QTimer::timeout, this, [this]() {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (lastLagSampleEpochMs_ > 0) {
            const qint64 lag = qMax<qint64>(0, now - lastLagSampleEpochMs_ - 1000);
            Telemetry::instance().setGauge("ui.event_loop_lag_ms", static_cast<double>(lag));
        }
        lastLagSampleEpochMs_ = now;
    });
    connect(memoryWatchTimer_, &QTimer::timeout, this, [this]() {
        const qint64 rssKb = processMemoryRssKb();
        Telemetry::instance().setGauge("memory.rss_kb", static_cast<double>(rssKb));
        Telemetry::instance().exportToFile(
            QDir(QDir::currentPath()).filePath("logs/telemetry_live.json"));
        if (rssKb > 800000) {
            refreshIntervalMs_ = qMin(maxRefreshIntervalMs_, refreshIntervalMs_ + 1000);
            pruneNodeParameterCache();
        }
    });
    eventLoopLagTimer_->start();
    memoryWatchTimer_->start();

    connect(modeCombo_, &QComboBox::currentTextChanged, this, [this]() {
        applyMode();
        scheduleRefresh(0, true);
    });
    connect(processSearch_, &QLineEdit::textChanged, this, [this]() {
        processOffset_ = 0;
        refreshDebounceTimer_->start();
    });
    connect(processScopeCombo_, &QComboBox::currentTextChanged, this, [this]() {
        processOffset_ = 0;
        refreshDebounceTimer_->start();
    });
    connect(processViewModeCombo_, &QComboBox::currentTextChanged, this, [this]() {
        applyProcessTableMode();
    });
    connect(tabs_, &QTabWidget::currentChanged, this, [this]() { scheduleRefresh(0, true); });
    connect(processPrevButton_, &QPushButton::clicked, this, [this]() {
        processOffset_ = qMax(0, processOffset_ - processLimit_);
        scheduleRefresh(0, true);
    });
    connect(processNextButton_, &QPushButton::clicked, this, [this]() {
        if (processOffset_ + processLimit_ < processTotalFiltered_) {
            processOffset_ += processLimit_;
            scheduleRefresh(0, true);
        }
    });

    connect(terminateButton_, &QPushButton::clicked, this, [this]() { runProcessAction("terminate_pid"); });
    connect(forceKillButton_, &QPushButton::clicked, this, [this]() { runProcessAction("kill_pid"); });
    connect(killTreeButton_, &QPushButton::clicked, this, [this]() { runProcessAction("kill_tree"); });

    connect(emergencyStopButton_, &QPushButton::clicked, this, [this]() {
        runGlobalAction("kill_all_ros");
    });
    connect(restartDomainButton_, &QPushButton::clicked, this, [this]() {
        runGlobalAction("restart_domain", {{"domain_id", selectedDomainId()}});
    });
    connect(isolateDomainButton_, &QPushButton::clicked, this, [this]() {
        runGlobalAction("isolate_domain", {{"domain_id", selectedDomainId()}});
    });
    connect(clearShmButton_, &QPushButton::clicked, this, [this]() {
        runGlobalAction("clear_shared_memory");
    });
    connect(restartWorkspaceButton_, &QPushButton::clicked, this, [this]() {
        if (workspacePathInput_->text().trimmed().isEmpty()) {
            showMessage("Workspace path is required.", true);
            return;
        }
        runGlobalAction(
            "restart_workspace",
            {
                {"workspace_path", workspacePathInput_->text().trimmed()},
                {"relaunch_command", workspaceRelaunchInput_->text().trimmed()},
            });
    });
    connect(snapshotJsonButton_, &QPushButton::clicked, this, [this]() { runGlobalAction("snapshot_json"); });
    connect(snapshotYamlButton_, &QPushButton::clicked, this, [this]() { runGlobalAction("snapshot_yaml"); });
    connect(compareSnapshotButton_, &QPushButton::clicked, this, [this]() {
        const QString leftPath = QFileDialog::getOpenFileName(
            this, "Select Older Snapshot", QDir::currentPath(), "JSON Files (*.json)");
        if (leftPath.isEmpty()) {
            return;
        }
        const QString rightPath = QFileDialog::getOpenFileName(
            this, "Select Newer Snapshot", QDir::currentPath(), "JSON Files (*.json)");
        if (rightPath.isEmpty()) {
            return;
        }
        runGlobalAction("compare_snapshots", {{"left_path", leftPath}, {"right_path", rightPath}});
    });
    connect(savePresetButton_, &QPushButton::clicked, this, [this]() {
        bool ok = false;
        const QString name = QInputDialog::getText(
            this, "Save Preset", "Preset name:", QLineEdit::Normal, "default", &ok);
        if (!ok) {
            return;
        }
        runGlobalAction("save_preset", {{"name", name}});
    });
    connect(loadPresetButton_, &QPushButton::clicked, this, [this]() {
        bool ok = false;
        const QString name = QInputDialog::getText(
            this, "Load Preset", "Preset name:", QLineEdit::Normal, "default", &ok);
        if (!ok) {
            return;
        }
        runGlobalAction("load_preset", {{"name", name}});
    });
    connect(sessionStartButton_, &QPushButton::clicked, this, [this]() {
        bool ok = false;
        const QString name = QInputDialog::getText(
            this, "Start Session Recorder", "Session name:", QLineEdit::Normal, "runtime_session", &ok);
        if (!ok) {
            return;
        }
        runGlobalAction("session_start", {{"session_name", name}});
    });
    connect(sessionStopButton_, &QPushButton::clicked, this, [this]() { runGlobalAction("session_stop"); });
    connect(sessionExportButton_, &QPushButton::clicked, this, [this]() {
        runGlobalAction("session_export", {{"format", "json"}});
    });
    connect(telemetryExportButton_, &QPushButton::clicked, this, [this]() {
        const QString defaultPath = QDir(QDir::currentPath()).filePath("logs/telemetry.json");
        const QString path = QFileDialog::getSaveFileName(
            this, "Export Telemetry", defaultPath, "JSON Files (*.json)");
        if (path.isEmpty()) {
            return;
        }
        runGlobalAction("export_telemetry", {{"path", path}});
    });
    connect(watchdogToggleButton_, &QPushButton::clicked, this, [this]() {
        const bool enabled = cachedWatchdog_.value("enabled").toBool(false);
        runGlobalAction(enabled ? "watchdog_disable" : "watchdog_enable");
    });
    connect(fleetRefreshButton_, &QPushButton::clicked, this, [this]() { runGlobalAction("fleet_refresh"); });
    connect(fleetLoadTargetsButton_, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(
            this, "Select Fleet Targets JSON", QDir::currentPath(), "JSON Files (*.json)");
        if (path.isEmpty()) {
            return;
        }
        runGlobalAction("fleet_load_targets", {{"path", path}});
    });
    connect(remoteRestartButton_, &QPushButton::clicked, this, [this]() {
        bool ok = false;
        const QString target = QInputDialog::getText(
            this, "Remote Restart", "Target name:", QLineEdit::Normal, "", &ok);
        if (!ok || target.trimmed().isEmpty()) {
            return;
        }
        const QString domain = QInputDialog::getText(
            this, "Remote Restart", "Domain ID:", QLineEdit::Normal, "0", &ok);
        if (!ok) {
            return;
        }
        runGlobalAction(
            "remote_action",
            {
                {"target", target.trimmed()},
                {"remote_action", "restart_domain"},
                {"domain_id", domain.trimmed()},
            });
    });
    connect(remoteKillButton_, &QPushButton::clicked, this, [this]() {
        bool ok = false;
        const QString target = QInputDialog::getText(
            this, "Remote Kill", "Target name:", QLineEdit::Normal, "", &ok);
        if (!ok || target.trimmed().isEmpty()) {
            return;
        }
        runGlobalAction(
            "remote_action",
            {
                {"target", target.trimmed()},
                {"remote_action", "kill_ros"},
                {"domain_id", "0"},
            });
    });

    connect(domainTable_, &QTableWidget::itemSelectionChanged, this, [this]() {
        currentDomain_ = selectedDomainId();
        scheduleRefresh(0, true);
    });

    connect(fetchParamsButton_, &QPushButton::clicked, this, [this]() {
        QTreeWidgetItem* item = nodesTree_->currentItem();
        if (item == nullptr) {
            showMessage("Select a node first.", true);
            return;
        }
        while (item->parent() != nullptr) {
            item = item->parent();
        }
        const QString nodeName = item->data(0, Qt::UserRole).toString();
        if (nodeName.isEmpty()) {
            showMessage("Select a node first.", true);
            return;
        }

        if (cachedNodeParameters_.contains(nodeName)) {
            paramsText_->setPlainText(cachedNodeParameters_.value(nodeName).toString());
            return;
        }

        emit nodeParametersRequested(selectedDomainId(), nodeName);
    });
}

void MainWindow::applyMode() {
    const bool engineer = modeCombo_->currentText() == "Engineer";
    terminateButton_->setVisible(engineer);
    forceKillButton_->setVisible(engineer);
    killTreeButton_->setVisible(engineer);
    restartDomainButton_->setVisible(engineer);
    isolateDomainButton_->setVisible(engineer);
    clearShmButton_->setVisible(engineer);
    restartWorkspaceButton_->setVisible(engineer);
    workspacePathInput_->setVisible(engineer);
    workspaceRelaunchInput_->setVisible(engineer);
    fetchParamsButton_->setVisible(engineer);
    paramsText_->setVisible(engineer);
    compareSnapshotButton_->setEnabled(engineer);
    savePresetButton_->setEnabled(engineer);
    loadPresetButton_->setEnabled(engineer);
    sessionStartButton_->setEnabled(engineer);
    sessionStopButton_->setEnabled(engineer);
    sessionExportButton_->setEnabled(engineer);
    telemetryExportButton_->setEnabled(engineer);
    fleetLoadTargetsButton_->setEnabled(engineer);
    fleetRefreshButton_->setEnabled(engineer);
    remoteRestartButton_->setEnabled(engineer);
    remoteKillButton_->setEnabled(engineer);
    watchdogToggleButton_->setEnabled(engineer);

    // Operator mode keeps only high-level runtime and hardware view.
    tabs_->setTabEnabled(2, engineer);
    tabs_->setTabEnabled(3, engineer);
    tabs_->setTabEnabled(5, engineer);
    tabs_->setTabEnabled(6, engineer);
    tabs_->setTabEnabled(7, engineer);
    tabs_->setTabEnabled(8, engineer);
    tabs_->setTabEnabled(9, engineer);
    tabs_->setTabEnabled(10, engineer);
}

QJsonObject MainWindow::buildPollRequest() const {
    QJsonObject request;
    request.insert("process_scope", processScopeCombo_->currentText());
    request.insert("ros_only", processScopeCombo_->currentText() == "ROS Only");
    request.insert("process_query", processSearch_->text().trimmed());
    request.insert("process_offset", processOffset_);
    request.insert("process_limit", processLimit_);
    request.insert("selected_domain", selectedDomainId());
    request.insert("engineer_mode", modeCombo_->currentText() == "Engineer");
    request.insert("active_tab", tabs_->currentIndex());
    request.insert("since_version", static_cast<double>(cachedSyncVersion_));
    request.insert("if_none_match", cachedEtag_);
    return request;
}

void MainWindow::scheduleRefresh(int delayMs, bool force) {
    if (refreshTimer_ == nullptr) {
        return;
    }
    if (refreshInFlight_ && !force) {
        return;
    }
    const int delay = qBound(0, delayMs, maxRefreshIntervalMs_);
    refreshTimer_->start(delay);
}

void MainWindow::queueRefresh() {
    if (refreshInFlight_) {
        return;
    }
    refreshInFlight_ = true;
    emit pollRequested(buildPollRequest());
}

QString MainWindow::selectedDomainId() const {
    const int row = domainTable_->currentRow();
    if (row >= 0 && domainTable_->item(row, 0) != nullptr) {
        return domainTable_->item(row, 0)->text();
    }
    if (!currentDomain_.isEmpty()) {
        return currentDomain_;
    }
    if (!cachedDomainSummaries_.isEmpty()) {
        return cachedDomainSummaries_.first().toObject().value("domain_id").toString("0");
    }
    return "0";
}

qint64 MainWindow::processMemoryRssKb() {
    QFile file("/proc/self/status");
    if (!file.open(QIODevice::ReadOnly)) {
        return -1;
    }
    const QString text = QString::fromUtf8(file.readAll());
    const QStringList lines = text.split('\n', Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        if (!line.startsWith("VmRSS:")) {
            continue;
        }
        const QStringList parts = line.split(' ', Qt::SkipEmptyParts);
        if (parts.size() >= 2) {
            return parts[1].toLongLong();
        }
    }
    return -1;
}

void MainWindow::updateProcessPaginationLabel() {
    if (processPageLabel_ == nullptr) {
        return;
    }
    const int start = processTotalFiltered_ == 0 ? 0 : processOffset_ + 1;
    const int end = qMin(processTotalFiltered_, processOffset_ + processLimit_);
    processPageLabel_->setText(QString("Rows %1-%2 / %3").arg(start).arg(end).arg(processTotalFiltered_));
    processPrevButton_->setEnabled(processOffset_ > 0);
    processNextButton_->setEnabled(processOffset_ + processLimit_ < processTotalFiltered_);
}

void MainWindow::pruneNodeParameterCache() {
    while (nodeParameterOrder_.size() > maxNodeParameterCache_) {
        const QString oldest = nodeParameterOrder_.takeFirst();
        cachedNodeParameters_.remove(oldest);
    }
}

void MainWindow::updateProcessScopeOptions() {
    if (processScopeCombo_ == nullptr) {
        return;
    }
    const QString current = processScopeCombo_->currentText();
    QSignalBlocker blocker(processScopeCombo_);
    processScopeCombo_->clear();
    processScopeCombo_->addItem("All Processes");
    processScopeCombo_->addItem("ROS Only");
    for (const QJsonValue& value : cachedDomainSummaries_) {
        const QString id = value.toObject().value("domain_id").toString("0");
        processScopeCombo_->addItem(QString("Domain %1").arg(id));
    }
    int idx = processScopeCombo_->findText(current);
    if (idx < 0) {
        idx = processScopeCombo_->findText("ROS Only");
    }
    processScopeCombo_->setCurrentIndex(qMax(0, idx));
}

void MainWindow::applyProcessTableMode() {
    if (processTable_ == nullptr || processViewModeCombo_ == nullptr) {
        return;
    }
    const bool compact = processViewModeCombo_->currentText() == "Compact View";
    processTable_->setColumnHidden(1, compact);   // PPID
    processTable_->setColumnHidden(5, compact);   // Threads
    processTable_->setColumnHidden(6, compact);   // Uptime
    processTable_->setColumnHidden(8, false);     // Node
    processTable_->setColumnHidden(9, compact);   // Executable
    processTable_->setColumnHidden(11, compact);  // Launch
}

void MainWindow::renderFromSnapshot(const QJsonObject& snapshot) {
    const QElapsedTimer renderTimer = []() {
        QElapsedTimer t;
        t.start();
        return t;
    }();

    if (snapshot.contains("processes_all")) {
        cachedProcessesAll_ = snapshot.value("processes_all").toArray();
    }
    if (snapshot.contains("processes_visible")) {
        cachedProcessesVisible_ = snapshot.value("processes_visible").toArray();
    }
    if (snapshot.contains("domain_summaries")) {
        cachedDomainSummaries_ = snapshot.value("domain_summaries").toArray();
        updateProcessScopeOptions();
    }
    if (snapshot.contains("domains")) {
        cachedDomains_ = snapshot.value("domains").toArray();
    }
    if (snapshot.contains("graph")) {
        cachedGraph_ = snapshot.value("graph").toObject();
    }
    if (snapshot.contains("tf_nav2")) {
        cachedTfNav2_ = snapshot.value("tf_nav2").toObject();
    }
    if (snapshot.contains("system")) {
        cachedSystem_ = snapshot.value("system").toObject();
    }
    if (snapshot.contains("health")) {
        cachedHealth_ = snapshot.value("health").toObject();
    }
    if (snapshot.contains("advanced")) {
        cachedAdvanced_ = snapshot.value("advanced").toObject();
    }
    if (snapshot.contains("fleet")) {
        cachedFleet_ = snapshot.value("fleet").toObject();
    }
    if (snapshot.contains("session")) {
        cachedSession_ = snapshot.value("session").toObject();
    }
    if (snapshot.contains("watchdog")) {
        cachedWatchdog_ = snapshot.value("watchdog").toObject();
    }
    if (snapshot.contains("logs")) {
        cachedLogs_ = snapshot.value("logs").toString();
    }
    if (snapshot.contains("node_parameters")) {
        cachedNodeParameters_ = snapshot.value("node_parameters").toObject();
    }
    if (snapshot.contains("selected_domain")) {
        currentDomain_ = snapshot.value("selected_domain").toString("0");
    }
    if (snapshot.contains("sync_version")) {
        cachedSyncVersion_ = static_cast<qint64>(snapshot.value("sync_version").toDouble(-1));
    }
    if (snapshot.contains("etag")) {
        cachedEtag_ = snapshot.value("etag").toString();
    }
    processTotalFiltered_ = snapshot.value("process_total_filtered").toInt(processTotalFiltered_);
    processOffset_ = snapshot.value("process_offset").toInt(processOffset_);
    processLimit_ = snapshot.value("process_limit").toInt(processLimit_);
    const int workerBackoff = snapshot.value("idle_backoff_ms").toInt(refreshIntervalMs_);
    refreshIntervalMs_ = qBound(minRefreshIntervalMs_, workerBackoff, maxRefreshIntervalMs_);
    Telemetry::instance().setQueueSize(
        "offline_remote_actions", snapshot.value("offline_queue_size").toInt(0));
    if (presetLabel_ != nullptr) {
        presetLabel_->setText(QString("Preset: %1").arg(snapshot.value("preset_name").toString("default")));
    }
    if (watchdogToggleButton_ != nullptr) {
        watchdogToggleButton_->setText(
            cachedWatchdog_.value("enabled").toBool(false) ? "Watchdog: ON" : "Watchdog: OFF");
    }

    renderProcesses();
    renderDomains();

    if (tabs_->isTabEnabled(2)) {
        renderNodesTopics();
    }
    if (tabs_->isTabEnabled(3)) {
        renderTfNav2();
    }
    renderSystemHardware();
    if (tabs_->isTabEnabled(5)) {
        renderLogs();
    }
    const int activeTab = tabs_->currentIndex();
    if (activeTab == 0) {
        refreshIntervalMs_ = qMax(refreshIntervalMs_, 2200);
    }
    if (diagnosticsText_ != nullptr && activeTab == 6) {
        const QJsonObject rate = cachedAdvanced_.value("topic_rate_analyzer").toObject();
        const QJsonObject qos = cachedAdvanced_.value("qos_mismatch_detector").toObject();
        const QJsonObject lifecycle = cachedAdvanced_.value("lifecycle_timeline").toObject();
        const QJsonObject leaks = cachedAdvanced_.value("memory_leak_detection").toObject();
        const QJsonObject net = cachedAdvanced_.value("network_saturation_monitor").toObject();
        const QJsonObject launch = cachedAdvanced_.value("deterministic_launch_validation").toObject();
        const QJsonObject impact = cachedAdvanced_.value("dependency_impact_map").toObject();
        QStringList lines;
        lines << "Diagnostics Overview";
        lines << "";
        lines << QString("Runtime Stability Score: %1")
                     .arg(cachedAdvanced_.value("runtime_stability_score").toInt(0));
        lines << QString("Topic Rate Issues: %1").arg(rate.value("issue_count").toInt(0));
        lines << QString("QoS Mismatches: %1").arg(qos.value("mismatch_count").toInt(0));
        lines << QString("Lifecycle Stuck Nodes: %1")
                     .arg(lifecycle.value("stuck_nodes").toArray().size());
        lines << QString("Memory Leak Candidates: %1").arg(leaks.value("candidate_count").toInt(0));
        lines << QString("Congested Interfaces: %1")
                     .arg(net.value("congested_interfaces").toArray().size());
        lines << QString("Launch Valid: %1")
                     .arg(launch.value("valid").toBool(true) ? "YES" : "NO");
        lines << "";
        lines << "Top Dependency Impact Nodes:";
        const QJsonArray top = impact.value("top_impact_nodes").toArray();
        if (top.isEmpty()) {
            lines << " - none";
        } else {
            const int limit = qMin(6, top.size());
            for (int i = 0; i < limit; ++i) {
                const QJsonObject row = top.at(i).toObject();
                lines << QString(" - %1 (downstream %2)")
                             .arg(row.value("node").toString())
                             .arg(row.value("downstream_count").toInt());
            }
        }
        diagnosticsText_->setPlainText(lines.join('\n'));
    }
    if (performanceText_ != nullptr && activeTab == 7) {
        const QJsonObject topicRates = cachedAdvanced_.value("topic_rate_analyzer").toObject();
        const QJsonObject leaks = cachedAdvanced_.value("memory_leak_detection").toObject();
        const QJsonObject network = cachedAdvanced_.value("network_saturation_monitor").toObject();
        const QJsonObject correlation = cachedAdvanced_.value("cross_correlation_timeline").toObject();
        QStringList lines;
        lines << "Performance Overview";
        lines << "";
        lines << QString("Runtime Stability Score: %1")
                     .arg(cachedAdvanced_.value("runtime_stability_score").toInt(0));
        lines << QString("Topic Rate Alerts: %1")
                     .arg(topicRates.value("issue_count").toInt(0));
        lines << QString("Memory Leak Candidates: %1")
                     .arg(leaks.value("candidate_count").toInt(0));
        lines << QString("High Traffic Topics: %1")
                     .arg(network.value("high_traffic_topic_count").toInt(0));
        lines << QString("Correlated Events: %1")
                     .arg(correlation.value("correlated_events").toArray().size());
        lines << "";
        lines << "Tip: switch to Diagnostics tab for deep details.";
        performanceText_->setPlainText(lines.join('\n'));
    }
    if (safetyText_ != nullptr && activeTab == 8) {
        const QJsonObject soft = cachedAdvanced_.value("soft_safety_boundary").toObject();
        const QJsonObject tfDrift = cachedAdvanced_.value("tf_drift_monitor").toObject();
        QStringList lines;
        lines << "Safety Overview";
        lines << "";
        lines << QString("Watchdog: %1")
                     .arg(cachedWatchdog_.value("enabled").toBool(false) ? "ON" : "OFF");
        lines << QString("Health State: %1")
                     .arg(cachedHealth_.value("status").toString("unknown").toUpper());
        lines << QString("Zombie Nodes: %1")
                     .arg(cachedHealth_.value("zombie_nodes").toArray().size());
        lines << QString("Domain Conflicts: %1")
                     .arg(cachedHealth_.value("domain_conflicts").toArray().size());
        lines << QString("Soft Boundary Warnings: %1")
                     .arg(soft.value("warning_count").toInt(0));
        lines << QString("TF Duplicate Children: %1")
                     .arg(tfDrift.value("duplicate_count").toInt(0));
        safetyText_->setPlainText(lines.join('\n'));
    }
    if (workspaceText_ != nullptr && activeTab == 9) {
        const QJsonObject ws = cachedAdvanced_.value("workspace_tools").toObject();
        const QJsonArray chain = ws.value("overlay_chain").toArray();
        const QJsonArray dup = ws.value("duplicate_packages").toArray();
        const QJsonArray distros = ws.value("detected_distributions").toArray();
        QStringList lines;
        lines << "Workspace Overview";
        lines << "";
        lines << QString("Overlay Count: %1").arg(chain.size());
        lines << QString("Duplicate Packages: %1").arg(dup.size());
        lines << QString("Mixed ROS Distributions: %1")
                     .arg(ws.value("mixed_ros_distributions").toBool(false) ? "YES" : "NO");
        lines << QString("ABI Mismatch Suspected: %1")
                     .arg(ws.value("abi_mismatch_suspected").toBool(false) ? "YES" : "NO");
        lines << "";
        lines << "Detected Distributions:";
        if (distros.isEmpty()) {
            lines << " - none";
        } else {
            for (const QJsonValue& v : distros) {
                lines << QString(" - %1").arg(v.toString());
            }
        }
        lines << "";
        lines << "Overlay Chain:";
        if (chain.isEmpty()) {
            lines << " - none";
        } else {
            for (const QJsonValue& v : chain) {
                lines << QString(" - %1").arg(v.toString());
            }
        }
        workspaceText_->setPlainText(lines.join('\n'));
    }
    if (fleetText_ != nullptr && activeTab == 10) {
        const QJsonArray robots = cachedFleet_.value("robots").toArray();
        QStringList lines;
        lines << "Fleet Overview";
        lines << "";
        lines << QString("Healthy Robots: %1 / %2")
                     .arg(cachedFleet_.value("healthy_count").toInt(0))
                     .arg(cachedFleet_.value("total_count").toInt(0));
        lines << QString("Offline Action Queue: %1")
                     .arg(cachedFleet_.value("offline_queue_size").toInt(0));
        lines << "";
        lines << "Robots:";
        for (const QJsonValue& value : robots) {
            const QJsonObject robot = value.toObject();
            const QString name = robot.value("name").toString(robot.value("host").toString("unknown"));
            const bool reachable = robot.value("reachable").toBool(false);
            lines << QString(" - %1: %2")
                         .arg(name, reachable ? "reachable" : "unreachable");
        }
        fleetText_->setPlainText(lines.join('\n'));
    }
    renderHealthSummary();
    updateProcessPaginationLabel();
    Telemetry::instance().recordDurationMs("ui.render.snapshot_ms", renderTimer.elapsed());
}

void MainWindow::renderProcesses() {
    QElapsedTimer timer;
    timer.start();
    const QString currentHash = hashArray(cachedProcessesVisible_);
    if (currentHash == lastProcessRenderHash_) {
        Telemetry::instance().recordDurationMs("ui.render.process_list_ms", timer.elapsed());
        return;
    }
    lastProcessRenderHash_ = currentHash;
    processTable_->setRowCount(cachedProcessesVisible_.size());

    QSet<QString> zombieNodes;
    for (const QJsonValue& value : cachedHealth_.value("zombie_nodes").toArray()) {
        zombieNodes.insert(value.toObject().value("node").toString());
    }
    QSet<QString> duplicateNodes;
    for (const QJsonValue& value : cachedHealth_.value("duplicate_nodes").toArray()) {
        duplicateNodes.insert(value.toString());
    }
    QSet<QString> inactiveLifecycleNodes;
    const QJsonArray lifecycle =
        cachedTfNav2_.value("runtime").toObject().value("lifecycle_states").toArray();
    for (const QJsonValue& value : lifecycle) {
        const QJsonObject row = value.toObject();
        const QString state = row.value("state").toString().toLower();
        if (state != "active") {
            inactiveLifecycleNodes.insert(row.value("node").toString());
        }
    }
    QSet<QString> mismatchTopics;
    const QJsonArray mismatches =
        cachedAdvanced_.value("qos_mismatch_detector").toObject().value("mismatches").toArray();
    for (const QJsonValue& value : mismatches) {
        mismatchTopics.insert(value.toObject().value("topic").toString());
    }
    QSet<QString> qosMismatchNodes;
    for (const QJsonValue& value : cachedGraph_.value("nodes").toArray()) {
        const QJsonObject node = value.toObject();
        const QString full = node.value("full_name").toString();
        auto collect = [&](const QJsonArray& entries) {
            for (const QJsonValue& entryValue : entries) {
                const QString topic = entryValue.toObject().value("name").toString();
                if (mismatchTopics.contains(topic)) {
                    qosMismatchNodes.insert(full);
                    return;
                }
            }
        };
        collect(node.value("publishers").toArray());
        collect(node.value("subscribers").toArray());
    }

    int row = 0;
    for (const QJsonValue& value : cachedProcessesVisible_) {
        const QJsonObject proc = value.toObject();
        processTable_->setItem(
            row, 0, new QTableWidgetItem(QString::number(static_cast<qint64>(proc.value("pid").toDouble()))));
        processTable_->setItem(
            row, 1, new QTableWidgetItem(QString::number(static_cast<qint64>(proc.value("ppid").toDouble()))));
        processTable_->setItem(row, 2, new QTableWidgetItem(proc.value("name").toString()));
        processTable_->setItem(
            row,
            3,
            new QTableWidgetItem(QString::number(proc.value("cpu_percent").toDouble(), 'f', 1)));
        processTable_->setItem(
            row,
            4,
            new QTableWidgetItem(QString::number(proc.value("memory_percent").toDouble(), 'f', 1)));
        processTable_->setItem(
            row, 5, new QTableWidgetItem(QString::number(proc.value("threads").toInt())));
        processTable_->setItem(row, 6, new QTableWidgetItem(proc.value("uptime_human").toString()));
        processTable_->setItem(
            row, 7, new QTableWidgetItem(proc.value("ros_domain_id").toString("0")));
        processTable_->setItem(row, 8, new QTableWidgetItem(proc.value("node_name").toString()));
        processTable_->setItem(row, 9, new QTableWidgetItem(proc.value("executable").toString()));
        processTable_->setItem(
            row, 10, new QTableWidgetItem(proc.value("workspace_origin").toString()));
        processTable_->setItem(row, 11, new QTableWidgetItem(proc.value("launch_source").toString()));

        const QString nodeName = proc.value("node_name").toString();
        const bool isRos = proc.value("is_ros").toBool(false);
        QColor background = QColor("#f3f7f3");
        QColor foreground = QColor("#111111");
        QString reason = "Healthy";
        if (!isRos) {
            background = QColor("#f7f7f7");
            foreground = QColor("#9a9a9a");
            reason = "Non-ROS process";
        } else if (zombieNodes.contains(nodeName)) {
            background = QColor("#ffe9e9");  // red
            reason = "Zombie node: PID missing or invalid";
        } else if (qosMismatchNodes.contains(nodeName)) {
            background = QColor("#fff7d6");  // yellow
            reason = "QoS mismatch detected for one or more node topics";
        } else if (duplicateNodes.contains(nodeName)) {
            background = QColor("#f3ecff");  // purple
            reason = "Duplicate node name detected";
        } else if (inactiveLifecycleNodes.contains(nodeName)) {
            background = QColor("#e8f1ff");  // blue
            reason = "Lifecycle node not in active state";
        }
        for (int c = 0; c < processTable_->columnCount(); ++c) {
            QTableWidgetItem* item = processTable_->item(row, c);
            if (item != nullptr) {
                item->setBackground(QBrush(background));
                item->setForeground(QBrush(foreground));
                item->setToolTip(reason);
            }
        }
        row++;
    }
    Telemetry::instance().recordDurationMs("ui.render.process_list_ms", timer.elapsed());
}

void MainWindow::renderDomains() {
    QElapsedTimer timer;
    timer.start();
    const QString currentHash = hashArray(cachedDomainSummaries_) + "|" + hashArray(cachedDomains_);
    if (currentHash == lastDomainRenderHash_) {
        Telemetry::instance().recordDurationMs("ui.render.domain_list_ms", timer.elapsed());
        return;
    }
    lastDomainRenderHash_ = currentHash;
    QSet<QString> conflictDomains;
    for (const QJsonValue& conflictValue : cachedHealth_.value("domain_conflicts").toArray()) {
        for (const QJsonValue& domainValue : conflictValue.toObject().value("domains").toArray()) {
            conflictDomains.insert(domainValue.toString());
        }
    }

    QSignalBlocker blocker(domainTable_);
    domainTable_->setRowCount(cachedDomainSummaries_.size());
    const QString tfDomain = cachedTfNav2_.value("domain_id").toString();
    const QJsonArray lifecycleStates =
        cachedTfNav2_.value("runtime").toObject().value("lifecycle_states").toArray();
    auto hasActive = [&](const QStringList& tokens, bool* foundOut) {
        bool found = false;
        bool allActive = true;
        for (const QJsonValue& v : lifecycleStates) {
            const QJsonObject rowObj = v.toObject();
            const QString node = rowObj.value("node").toString().toLower();
            const QString state = rowObj.value("state").toString().toLower();
            for (const QString& token : tokens) {
                if (node.contains(token)) {
                    found = true;
                    if (state != "active") {
                        allActive = false;
                    }
                }
            }
        }
        if (foundOut != nullptr) {
            *foundOut = found;
        }
        return allActive;
    };

    int row = 0;
    for (const QJsonValue& value : cachedDomainSummaries_) {
        const QJsonObject summary = value.toObject();
        const QString domain = summary.value("domain_id").toString("0");

        int nodeCount = 0;
        for (const QJsonValue& domainValue : cachedDomains_) {
            const QJsonObject detail = domainValue.toObject();
            if (detail.value("domain_id").toString("0") == domain) {
                nodeCount = detail.value("nodes").toArray().size();
                break;
            }
        }

        domainTable_->setItem(row, 0, new QTableWidgetItem(domain));
        domainTable_->setItem(
            row, 1, new QTableWidgetItem(QString::number(summary.value("ros_process_count").toInt())));
        domainTable_->setItem(
            row,
            2,
            new QTableWidgetItem(
                QString::number(summary.value("domain_cpu_percent").toDouble(), 'f', 1)));
        domainTable_->setItem(
            row,
            3,
            new QTableWidgetItem(
                QString::number(summary.value("domain_memory_percent").toDouble(), 'f', 1)));
        domainTable_->setItem(row, 4, new QTableWidgetItem(QString::number(nodeCount)));

        const bool conflict = conflictDomains.contains(domain);
        auto* conflictItem = new QTableWidgetItem(conflict ? "YES" : "NO");
        if (conflict) {
            conflictItem->setForeground(QBrush(QColor("#b22222")));
            for (int c = 0; c < domainTable_->columnCount(); ++c) {
                QTableWidgetItem* item = domainTable_->item(row, c);
                if (item != nullptr) {
                    item->setBackground(QBrush(QColor("#fff2f0")));
                }
            }
        }
        domainTable_->setItem(row, 5, conflictItem);

        QString tfSlamStatus = "-";
        if (domain == tfDomain) {
            bool nav2Found = false;
            bool slamFound = false;
            const bool nav2Active = hasActive(
                {"nav2", "controller_server", "planner_server", "bt_navigator", "map_server", "amcl"},
                &nav2Found);
            const bool slamActive = hasActive({"slam", "slam_toolbox", "cartographer"}, &slamFound);
            const QString nav2State = !nav2Found ? "N/A" : (nav2Active ? "OK" : "WARN");
            const QString slamState = !slamFound ? "N/A" : (slamActive ? "OK" : "WARN");
            tfSlamStatus = QString("NAV2:%1 SLAM:%2").arg(nav2State, slamState);
        }
        domainTable_->setItem(row, 6, new QTableWidgetItem(tfSlamStatus));
        row++;
    }

    QString domainToSelect = currentDomain_;
    if (domainToSelect.isEmpty()) {
        domainToSelect = selectedDomainId();
    }
    for (int i = 0; i < domainTable_->rowCount(); ++i) {
        if (domainTable_->item(i, 0) != nullptr && domainTable_->item(i, 0)->text() == domainToSelect) {
            domainTable_->selectRow(i);
            break;
        }
    }

    QJsonArray nodes;
    const QString domainId = selectedDomainId();
    for (const QJsonValue& value : cachedDomains_) {
        const QJsonObject domain = value.toObject();
        if (domain.value("domain_id").toString("0") == domainId) {
            nodes = domain.value("nodes").toArray();
            break;
        }
    }

    domainNodeTable_->setRowCount(nodes.size());
    int n = 0;
    for (const QJsonValue& value : nodes) {
        const QJsonObject node = value.toObject();
        domainNodeTable_->setItem(n, 0, new QTableWidgetItem(node.value("node_name").toString()));
        domainNodeTable_->setItem(n, 1, new QTableWidgetItem(node.value("namespace").toString()));
        domainNodeTable_->setItem(n, 2, new QTableWidgetItem(QString::number(node.value("pid").toInt(-1))));
        domainNodeTable_->setItem(n, 3, new QTableWidgetItem(node.value("executable").toString()));
        domainNodeTable_->setItem(n, 4, new QTableWidgetItem(node.value("package").toString()));
        domainNodeTable_->setItem(
            n, 5, new QTableWidgetItem(node.value("workspace_origin").toString()));
        domainNodeTable_->setItem(n, 6, new QTableWidgetItem(node.value("launch_source").toString()));
        n++;
    }
    Telemetry::instance().recordDurationMs("ui.render.domain_list_ms", timer.elapsed());
}

void MainWindow::renderNodesTopics() {
    nodesTree_->clear();
    const QJsonObject topicQos = cachedGraph_.value("topic_qos").toObject();

    for (const QJsonValue& nodeValue : cachedGraph_.value("nodes").toArray()) {
        const QJsonObject node = nodeValue.toObject();
        const QString role = node.value("primary_behavior_role").toString("generic");
        const QString runtimeClass = node.value("runtime_classification").toString("idle");
        auto* nodeItem =
            new QTreeWidgetItem(
                nodesTree_,
                {node.value("full_name").toString(), "Node", role, runtimeClass});
        nodeItem->setData(0, Qt::UserRole, node.value("full_name").toString());

        const auto addCategory = [&](const QString& label, const QJsonArray& entries) {
            for (const QJsonValue& entryValue : entries) {
                const QJsonObject entry = entryValue.toObject();
                const QString topic = entry.value("name").toString();
                QString qos;
                if (topicQos.contains(topic)) {
                    const QJsonObject qosObj = topicQos.value(topic).toObject();
                    const QJsonArray profiles = qosObj.value("qos_profiles").toArray();
                    if (!profiles.isEmpty()) {
                        const QJsonObject first = profiles.first().toObject();
                        qos = QString("%1 | %2")
                                  .arg(first.value("reliability").toString())
                                  .arg(first.value("durability").toString());
                    }
                }
                new QTreeWidgetItem(
                    nodeItem,
                    {
                        "",
                        label,
                        topic,
                        entry.value("type").toString() + (qos.isEmpty() ? "" : " / " + qos),
                    });
            }
        };

        addCategory("Publisher", node.value("publishers").toArray());
        addCategory("Subscriber", node.value("subscribers").toArray());
        addCategory("Service Server", node.value("service_servers").toArray());
        addCategory("Service Client", node.value("service_clients").toArray());
        addCategory("Action Server", node.value("action_servers").toArray());
        addCategory("Action Client", node.value("action_clients").toArray());
    }
    nodesTree_->expandAll();

    auto addPreview = [](QStringList* lines, const QString& title, const QJsonArray& arr) {
        lines->append(QString("%1: %2").arg(title).arg(arr.size()));
        const int limit = qMin(5, arr.size());
        for (int i = 0; i < limit; ++i) {
            const QJsonValue value = arr.at(i);
            if (value.isObject()) {
                const QJsonObject obj = value.toObject();
                if (obj.contains("topic")) {
                    lines->append(QString(" - %1").arg(obj.value("topic").toString()));
                } else if (obj.contains("node")) {
                    lines->append(QString(" - %1").arg(obj.value("node").toString()));
                } else {
                    lines->append(" - issue");
                }
            } else {
                lines->append(QString(" - %1").arg(value.toString()));
            }
        }
    };

    const QJsonObject qosState = cachedAdvanced_.value("qos_mismatch_detector").toObject();
    const QJsonArray qosMismatches = qosState.value("mismatches").toArray();
    QStringList lines;
    lines << "QoS and Graph Alerts";
    lines << "";
    addPreview(&lines, "QoS mismatches", qosMismatches);
    lines << "";
    addPreview(&lines, "Publishers without subscribers",
               cachedGraph_.value("publishers_without_subscribers").toArray());
    lines << "";
    addPreview(&lines, "Subscribers without publishers",
               cachedGraph_.value("subscribers_without_publishers").toArray());
    lines << "";
    addPreview(&lines, "Missing service servers",
               cachedGraph_.value("missing_service_servers").toArray());
    lines << "";
    addPreview(&lines, "Missing action servers",
               cachedGraph_.value("missing_action_servers").toArray());
    lines << "";
    addPreview(&lines, "Duplicate node names",
               cachedGraph_.value("duplicate_node_names").toArray());
    qosText_->setPlainText(lines.join('\n'));
}

void MainWindow::renderTfNav2() {
    const QJsonArray edges = cachedTfNav2_.value("tf_edges").toArray();
    tfTable_->setRowCount(edges.size());
    int row = 0;
    for (const QJsonValue& value : edges) {
        const QJsonObject edge = value.toObject();
        tfTable_->setItem(row, 0, new QTableWidgetItem(edge.value("parent").toString()));
        tfTable_->setItem(row, 1, new QTableWidgetItem(edge.value("child").toString()));
        row++;
    }

    QHash<QString, QStringList> childrenByParent;
    QSet<QString> allParents;
    QSet<QString> allChildren;
    for (const QJsonValue& value : edges) {
        const QJsonObject edge = value.toObject();
        const QString parent = edge.value("parent").toString();
        const QString child = edge.value("child").toString();
        if (parent.isEmpty() || child.isEmpty()) {
            continue;
        }
        childrenByParent[parent].append(child);
        allParents.insert(parent);
        allChildren.insert(child);
    }

    QStringList roots = (allParents - allChildren).values();
    std::sort(roots.begin(), roots.end());
    QStringList lines;
    lines << "TF Tree";
    lines << "";
    if (roots.isEmpty()) {
        lines << "No TF roots detected.";
    } else {
        for (const QString& root : roots) {
            lines << root;
            QStringList queue = {root};
            QSet<QString> visited;
            while (!queue.isEmpty()) {
                const QString node = queue.takeFirst();
                if (visited.contains(node)) {
                    continue;
                }
                visited.insert(node);
                QStringList children = childrenByParent.value(node);
                std::sort(children.begin(), children.end());
                for (const QString& child : children) {
                    lines << QString("  -> %1").arg(child);
                    queue.append(child);
                }
            }
            lines << "";
        }
    }
    nav2Text_->setPlainText(lines.join('\n'));
}

void MainWindow::renderSystemHardware() {
    const QJsonObject cpu = cachedSystem_.value("cpu").toObject();
    const QJsonObject mem = cachedSystem_.value("memory").toObject();
    const QJsonObject disk = cachedSystem_.value("disk").toObject();
    const QJsonArray gpus = cachedSystem_.value("gpus").toArray();

    cpuLabel_->setText(QString("CPU: %1%").arg(cpu.value("usage_percent").toDouble(), 0, 'f', 1));
    memLabel_->setText(QString("Memory: %1%").arg(mem.value("used_percent").toDouble(), 0, 'f', 1));
    diskLabel_->setText(QString("Disk: %1%").arg(disk.value("used_percent").toDouble(), 0, 'f', 1));
    if (gpus.isEmpty()) {
        gpuLabel_->setText("GPU: unavailable");
    } else {
        const QJsonObject gpu0 = gpus.first().toObject();
        gpuLabel_->setText(
            QString("GPU: %1%").arg(gpu0.value("utilization_percent").toDouble(), 0, 'f', 1));
    }

    usbText_->setPlainText(joinArrayLines(cachedSystem_.value("usb_devices").toArray()));
    serialText_->setPlainText(joinArrayLines(cachedSystem_.value("serial_ports").toArray()));
    canText_->setPlainText(joinArrayLines(cachedSystem_.value("can_interfaces").toArray()));
    netText_->setPlainText(joinArrayLines(cachedSystem_.value("network_interfaces").toArray()));
}

void MainWindow::renderLogs() {
    logsText_->setPlainText(cachedLogs_);
}

void MainWindow::renderHealthSummary() {
    const QString status = cachedHealth_.value("status").toString("unknown").toLower();
    if (status == "critical") {
        healthLabel_->setStyleSheet(
            "font-size:18px;font-weight:800;padding:8px 12px;border-radius:10px;background:#fde8e8;color:#b22222;");
    } else if (status == "warning") {
        healthLabel_->setStyleSheet(
            "font-size:18px;font-weight:800;padding:8px 12px;border-radius:10px;background:#fff7da;color:#b8860b;");
    } else {
        healthLabel_->setStyleSheet(
            "font-size:18px;font-weight:800;padding:8px 12px;border-radius:10px;background:#e8f7ec;color:#1e8e3e;");
    }

    QString badge = "HEALTHY";
    if (status == "critical") {
        badge = "CRITICAL";
    } else if (status == "warning") {
        badge = "DEGRADED";
    }
    healthLabel_->setText(
        QString("%1 | Score %2 | Warnings %3 | Zombies %4")
            .arg(badge)
            .arg(cachedAdvanced_.value("runtime_stability_score").toInt(0))
            .arg(cachedHealth_.value("domain_conflicts").toArray().size()
                 + cachedHealth_.value("tf_warnings").toArray().size())
            .arg(cachedHealth_.value("zombie_nodes").toArray().size()));
}

void MainWindow::runProcessAction(const QString& action) {
    const int row = processTable_->currentRow();
    const qint64 pid = pidFromRow(processTable_, row);
    if (pid <= 0) {
        showMessage("Select a process row first.", true);
        return;
    }
    runGlobalAction(action, {{"pid", pid}});
}

void MainWindow::runGlobalAction(const QString& action, const QJsonObject& payload) {
    emit actionRequested(action, payload);
}

void MainWindow::showMessage(const QString& message, bool error) const {
    if (error) {
        statusBar()->showMessage("ERROR: " + message, 8000);
    } else {
        statusBar()->showMessage(message, 5000);
    }
}

}  // namespace rrcc
