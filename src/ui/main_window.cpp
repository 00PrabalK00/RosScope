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
#include <QSet>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QTableWidgetItem>
#include <QVBoxLayout>

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

    central_ = new QWidget(this);
    setCentralWidget(central_);

    auto* root = new QVBoxLayout(central_);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    auto* header = new QHBoxLayout();
    auto* title = new QLabel("Roscoppe");
    title->setStyleSheet("font-size: 20px; font-weight: 700;");
    auto* maintainer = new QLabel("Built and maintained by Prabal Khare");
    maintainer->setStyleSheet("color:#606060;");

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
    watchdogToggleButton_ = new QPushButton("Watchdog: OFF");
    fleetRefreshButton_ = new QPushButton("Fleet Refresh");
    fleetLoadTargetsButton_ = new QPushButton("Load Fleet Targets");
    remoteRestartButton_ = new QPushButton("Remote Restart");
    remoteKillButton_ = new QPushButton("Remote Kill");
    healthLabel_ = new QLabel("Health: UNKNOWN");
    healthLabel_->setStyleSheet("font-weight:700;color:#606060;");
    presetLabel_ = new QLabel("Preset: default");

    header->addWidget(title);
    header->addWidget(maintainer);
    header->addStretch(1);
    header->addWidget(new QLabel("Mode"));
    header->addWidget(modeCombo_);
    header->addWidget(refreshButton);
    header->addWidget(snapshotJsonButton_);
    header->addWidget(snapshotYamlButton_);
    header->addWidget(compareSnapshotButton_);
    header->addWidget(savePresetButton_);
    header->addWidget(loadPresetButton_);
    header->addWidget(sessionStartButton_);
    header->addWidget(sessionStopButton_);
    header->addWidget(sessionExportButton_);
    header->addWidget(watchdogToggleButton_);
    header->addWidget(fleetLoadTargetsButton_);
    header->addWidget(fleetRefreshButton_);
    header->addWidget(remoteRestartButton_);
    header->addWidget(remoteKillButton_);
    header->addWidget(emergencyStopButton_);
    header->addWidget(presetLabel_);
    header->addWidget(healthLabel_);
    root->addLayout(header);

    tabs_ = new QTabWidget();
    root->addWidget(tabs_, 1);

    auto* processTab = new QWidget();
    auto* processLayout = new QVBoxLayout(processTab);
    auto* processControls = new QHBoxLayout();
    processSearch_ = new QLineEdit();
    processSearch_->setPlaceholderText("Search by PID, name, executable, or command");
    rosOnlyCheck_ = new QCheckBox("ROS only");
    terminateButton_ = new QPushButton("SIGTERM");
    forceKillButton_ = new QPushButton("SIGKILL");
    killTreeButton_ = new QPushButton("Kill Tree");
    processControls->addWidget(processSearch_, 1);
    processControls->addWidget(rosOnlyCheck_);
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
    tabs_->addTab(processTab, "Processes");

    auto* domainTab = new QWidget();
    auto* domainLayout = new QVBoxLayout(domainTab);
    domainTable_ = new QTableWidget();
    domainTable_->setColumnCount(6);
    domainTable_->setHorizontalHeaderLabels(
        {"Domain", "ROS Processes", "CPU %", "Mem %", "Nodes", "Conflict"});
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
    nav2Text_->setPlaceholderText("Nav2 lifecycle states and TF warnings...");
    tfLayout->addWidget(tfTable_, 2);
    tfLayout->addWidget(nav2Text_, 1);
    tabs_->addTab(tfTab, "TF & Nav2");

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

    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(2000);
    statusBar()->showMessage("Ready");

    connect(refreshButton, &QPushButton::clicked, this, [this]() { queueRefresh(); });
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
        renderFromSnapshot(snapshot);
    });
    connect(worker_, &RuntimeWorker::actionFinished, this, [this](const QJsonObject& result) {
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
    });
    connect(worker_, &RuntimeWorker::nodeParametersReady, this, [this](const QJsonObject& result) {
        const QString node = result.value("node").toString();
        if (result.value("success").toBool(false)) {
            const QString parameters = result.value("parameters").toString();
            cachedNodeParameters_.insert(node, parameters);
            paramsText_->setPlainText(parameters);
            showMessage(QString("Loaded parameters for %1").arg(node));
        } else {
            paramsText_->setPlainText(result.value("error").toString());
            showMessage(QString("Failed to load parameters for %1").arg(node), true);
        }
    });

    connect(refreshTimer_, &QTimer::timeout, this, [this]() { queueRefresh(); });
    refreshTimer_->start();

    connect(modeCombo_, &QComboBox::currentTextChanged, this, [this]() {
        applyMode();
        queueRefresh();
    });
    connect(processSearch_, &QLineEdit::textChanged, this, [this]() { queueRefresh(); });
    connect(rosOnlyCheck_, &QCheckBox::toggled, this, [this]() { queueRefresh(); });
    connect(tabs_, &QTabWidget::currentChanged, this, [this]() { queueRefresh(); });

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
        queueRefresh();
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
    compareSnapshotButton_->setVisible(engineer);
    savePresetButton_->setVisible(engineer);
    loadPresetButton_->setVisible(engineer);
    sessionStartButton_->setVisible(engineer);
    sessionStopButton_->setVisible(engineer);
    sessionExportButton_->setVisible(engineer);
    watchdogToggleButton_->setVisible(engineer);
    fleetLoadTargetsButton_->setVisible(engineer);
    fleetRefreshButton_->setVisible(engineer);
    remoteRestartButton_->setVisible(engineer);
    remoteKillButton_->setVisible(engineer);

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
    request.insert("ros_only", rosOnlyCheck_->isChecked());
    request.insert("process_query", processSearch_->text().trimmed());
    request.insert("selected_domain", selectedDomainId());
    request.insert("engineer_mode", modeCombo_->currentText() == "Engineer");
    request.insert("active_tab", tabs_->currentIndex());
    return request;
}

void MainWindow::queueRefresh() {
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

void MainWindow::renderFromSnapshot(const QJsonObject& snapshot) {
    cachedProcessesAll_ = snapshot.value("processes_all").toArray();
    cachedProcessesVisible_ = snapshot.value("processes_visible").toArray();
    cachedDomainSummaries_ = snapshot.value("domain_summaries").toArray();
    cachedDomains_ = snapshot.value("domains").toArray();
    cachedGraph_ = snapshot.value("graph").toObject();
    cachedTfNav2_ = snapshot.value("tf_nav2").toObject();
    cachedSystem_ = snapshot.value("system").toObject();
    cachedHealth_ = snapshot.value("health").toObject();
    cachedAdvanced_ = snapshot.value("advanced").toObject();
    cachedFleet_ = snapshot.value("fleet").toObject();
    cachedSession_ = snapshot.value("session").toObject();
    cachedWatchdog_ = snapshot.value("watchdog").toObject();
    cachedLogs_ = snapshot.value("logs").toString();
    cachedNodeParameters_ = snapshot.value("node_parameters").toObject();
    currentDomain_ = snapshot.value("selected_domain").toString("0");
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
    if (diagnosticsText_ != nullptr) {
        diagnosticsText_->setPlainText(jsonToPretty(cachedAdvanced_));
    }
    if (performanceText_ != nullptr) {
        QJsonObject perf;
        perf.insert("topic_rate_analyzer", cachedAdvanced_.value("topic_rate_analyzer"));
        perf.insert("cross_correlation_timeline", cachedAdvanced_.value("cross_correlation_timeline"));
        perf.insert("memory_leak_detection", cachedAdvanced_.value("memory_leak_detection"));
        perf.insert("network_saturation_monitor", cachedAdvanced_.value("network_saturation_monitor"));
        perf.insert("runtime_stability_score", cachedAdvanced_.value("runtime_stability_score"));
        performanceText_->setPlainText(jsonToPretty(perf));
    }
    if (safetyText_ != nullptr) {
        QJsonObject safety;
        safety.insert("watchdog", cachedWatchdog_);
        safety.insert("health", cachedHealth_);
        safety.insert("soft_safety_boundary", cachedAdvanced_.value("soft_safety_boundary"));
        safety.insert("tf_drift_monitor", cachedAdvanced_.value("tf_drift_monitor"));
        safetyText_->setPlainText(jsonToPretty(safety));
    }
    if (workspaceText_ != nullptr) {
        workspaceText_->setPlainText(
            jsonToPretty(cachedAdvanced_.value("workspace_tools").toObject()));
    }
    if (fleetText_ != nullptr) {
        QJsonObject fleet;
        fleet.insert("fleet", cachedFleet_);
        fleet.insert("session", cachedSession_);
        fleetText_->setPlainText(jsonToPretty(fleet));
    }
    renderHealthSummary();
}

void MainWindow::renderProcesses() {
    processTable_->setRowCount(cachedProcessesVisible_.size());
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
        row++;
    }
}

void MainWindow::renderDomains() {
    QSet<QString> conflictDomains;
    for (const QJsonValue& conflictValue : cachedHealth_.value("domain_conflicts").toArray()) {
        for (const QJsonValue& domainValue : conflictValue.toObject().value("domains").toArray()) {
            conflictDomains.insert(domainValue.toString());
        }
    }

    QSignalBlocker blocker(domainTable_);
    domainTable_->setRowCount(cachedDomainSummaries_.size());
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

    QJsonObject alertBundle;
    alertBundle.insert(
        "publishers_without_subscribers", cachedGraph_.value("publishers_without_subscribers"));
    alertBundle.insert(
        "subscribers_without_publishers", cachedGraph_.value("subscribers_without_publishers"));
    alertBundle.insert("missing_service_servers", cachedGraph_.value("missing_service_servers"));
    alertBundle.insert("missing_action_servers", cachedGraph_.value("missing_action_servers"));
    alertBundle.insert("misinitialized_processes", cachedGraph_.value("misinitialized_processes"));
    alertBundle.insert("isolated_nodes", cachedGraph_.value("isolated_nodes"));
    alertBundle.insert("circular_dependencies", cachedGraph_.value("circular_dependencies"));
    alertBundle.insert("duplicate_node_names", cachedGraph_.value("duplicate_node_names"));
    alertBundle.insert("tf_warnings", cachedGraph_.value("tf_warnings"));
    alertBundle.insert("topic_qos", cachedGraph_.value("topic_qos"));
    alertBundle.insert("zombie_nodes", cachedHealth_.value("zombie_nodes"));
    qosText_->setPlainText(jsonToPretty(alertBundle));
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

    QJsonObject navBundle;
    navBundle.insert("warnings", cachedTfNav2_.value("tf_warnings"));
    navBundle.insert("nav2", cachedTfNav2_.value("nav2"));
    nav2Text_->setPlainText(jsonToPretty(navBundle));
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
        healthLabel_->setStyleSheet("font-weight:700;color:#b22222;");
    } else if (status == "warning") {
        healthLabel_->setStyleSheet("font-weight:700;color:#b8860b;");
    } else {
        healthLabel_->setStyleSheet("font-weight:700;color:#1e8e3e;");
    }

    healthLabel_->setText(
        QString("Health: %1 | Stability: %2 | Watchdog: %3 | Zombies: %4 | Conflicts: %5")
            .arg(status.toUpper())
            .arg(cachedAdvanced_.value("runtime_stability_score").toInt(0))
            .arg(cachedWatchdog_.value("enabled").toBool(false) ? "ON" : "OFF")
            .arg(cachedHealth_.value("zombie_nodes").toArray().size())
            .arg(cachedHealth_.value("domain_conflicts").toArray().size()));
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
