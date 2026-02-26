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
#include <QIcon>
#include <QStyle>
#include <QVBoxLayout>

#include "rrcc/telemetry.hpp"

namespace rrcc {

namespace {

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

QString boolText(bool value) {
    return value ? "Yes" : "No";
}

QString formatBytes(double bytes) {
    const double absBytes = qAbs(bytes);
    if (absBytes >= 1024.0 * 1024.0 * 1024.0) {
        return QString("%1 GB").arg(bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
    }
    if (absBytes >= 1024.0 * 1024.0) {
        return QString("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
    }
    if (absBytes >= 1024.0) {
        return QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    }
    return QString("%1 B").arg(bytes, 0, 'f', 0);
}

QString formatNetworkInterfaces(const QJsonArray& interfaces) {
    if (interfaces.isEmpty()) {
        return "No network interfaces detected.";
    }
    QStringList lines;
    for (const QJsonValue& value : interfaces) {
        const QJsonObject iface = value.toObject();
        const QString name = iface.value("name").toString("unknown");
        const bool up = iface.value("is_up").toBool(false);
        const bool running = iface.value("is_running").toBool(false);
        const double rxBytes = iface.value("rx_bytes").toDouble(0.0);
        const double txBytes = iface.value("tx_bytes").toDouble(0.0);
        const QJsonArray addresses = iface.value("addresses").toArray();
        QStringList addrList;
        addrList.reserve(addresses.size());
        for (const QJsonValue& addr : addresses) {
            addrList.append(addr.toString());
        }
        const QString addrText = addrList.isEmpty() ? "-" : addrList.join(", ");
        lines << QString("%1 | up:%2 running:%3 | rx:%4 tx:%5")
                     .arg(name, up ? "yes" : "no", running ? "yes" : "no", formatBytes(rxBytes), formatBytes(txBytes));
        lines << QString("  addresses: %1").arg(addrText);
    }
    return lines.join('\n');
}

void populateKeyValueTable(
    QTableWidget* table,
    const QVector<QPair<QString, QString>>& rows,
    const QSet<int>& warningRows = {},
    const QSet<int>& criticalRows = {}) {
    if (table == nullptr) {
        return;
    }
    table->clearContents();
    table->setRowCount(rows.size());
    for (int row = 0; row < rows.size(); ++row) {
        auto* keyItem = new QTableWidgetItem(rows.at(row).first);
        auto* valueItem = new QTableWidgetItem(rows.at(row).second);
        if (criticalRows.contains(row)) {
            const QColor bg("#432125");
            const QColor fg("#ffd6da");
            keyItem->setBackground(QBrush(bg));
            valueItem->setBackground(QBrush(bg));
            keyItem->setForeground(QBrush(fg));
            valueItem->setForeground(QBrush(fg));
        } else if (warningRows.contains(row)) {
            const QColor bg("#3f3823");
            const QColor fg("#ffefc7");
            keyItem->setBackground(QBrush(bg));
            valueItem->setBackground(QBrush(bg));
            keyItem->setForeground(QBrush(fg));
            valueItem->setForeground(QBrush(fg));
        }
        table->setItem(row, 0, keyItem);
        table->setItem(row, 1, valueItem);
    }
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

QString readLocalTextFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

void appendHistory(QVector<double>* values, double sample, int maxSize = 40) {
    values->append(sample);
    while (values->size() > maxSize) {
        values->removeFirst();
    }
}

QString sparkline(const QVector<double>& values, double maxValue = 100.0) {
    static const char* blocks[] = {" ", ".", ":", "-", "=", "+", "*", "#", "%", "@"};
    if (values.isEmpty()) {
        return "(no data)";
    }
    double scaleMax = maxValue;
    for (double v : values) {
        if (v > scaleMax) {
            scaleMax = v;
        }
    }
    scaleMax = qMax(1.0, scaleMax);

    QString out;
    out.reserve(values.size());
    for (double v : values) {
        const double norm = qBound(0.0, v / scaleMax, 1.0);
        const int idx = qBound(0, static_cast<int>(norm * 9.0), 9);
        out += blocks[idx];
    }
    return out;
}

QIcon themedIcon(const QWidget* widget, const QString& themeName, QStyle::StandardPixmap fallback) {
    const QIcon icon = QIcon::fromTheme(themeName);
    if (!icon.isNull()) {
        return icon;
    }
    if (widget != nullptr) {
        return widget->style()->standardIcon(fallback);
    }
    return {};
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
    setWindowTitle("RosScope");
    resize(1560, 940);
    setMinimumSize(1100, 700);
    setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);

    central_ = new QWidget(this);
    setCentralWidget(central_);
    setStyleSheet(
        "QWidget { background:#171b20; color:#e3e7ec; font-size:13px; }"
        "QLabel { color:#e3e7ec; }"
        "QPushButton { background:#2a3138; color:#f0f3f6; border:1px solid #3a444f; border-radius:8px; padding:6px 10px; }"
        "QPushButton:hover { background:#333c45; }"
        "QPushButton:pressed { background:#242b32; }"
        "QPushButton:disabled { background:#21272d; color:#8f9ba7; border-color:#313941; }"
        "QToolButton { background:#2a3138; color:#f0f3f6; border:1px solid #3a444f; border-radius:8px; padding:6px 10px; }"
        "QToolButton:hover { background:#333c45; }"
        "QLineEdit, QComboBox, QPlainTextEdit, QTreeWidget, QTableWidget {"
        "  background:#1f252c; color:#e3e7ec; border:1px solid #3a444f; border-radius:8px; selection-background-color:#4d6a55; }"
        "QTableWidget {"
        "  background:#1f252c; alternate-background-color:#26303a; color:#e6edf5; gridline-color:#34414d; }"
        "QTableWidget::item { color:#e6edf5; padding:4px; }"
        "QTableWidget::item:selected { background:#3d5c4c; color:#f4f9ff; }"
        "QTreeWidget { background:#1f252c; alternate-background-color:#26303a; color:#e6edf5; }"
        "QTreeWidget::item { color:#e6edf5; }"
        "QTreeWidget::item:selected { background:#3d5c4c; color:#f4f9ff; }"
        "QTabWidget::pane { border:1px solid #3a444f; border-radius:10px; background:#1b2127; }"
        "QTabBar::tab { background:#272f37; color:#c7d0d9; border:1px solid #3a444f; border-bottom:none; padding:8px 12px; margin-right:2px; border-top-left-radius:8px; border-top-right-radius:8px; }"
        "QTabBar::tab:selected { background:#34404a; color:#f4f7fa; }"
        "QHeaderView::section { background:#2a3138; color:#d3dce5; border:1px solid #3a444f; padding:6px; }"
        "QStatusBar { background:#1d232a; color:#9faebb; border-top:1px solid #3a444f; }");

    auto* root = new QVBoxLayout(central_);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    auto* header = new QHBoxLayout();
    auto* title = new QLabel("RosScope");
    title->setStyleSheet("font-size: 20px; font-weight: 700;");
    auto* aboutButton = new QPushButton("About");
    aboutButton->setIcon(themedIcon(this, "help-about", QStyle::SP_MessageBoxInformation));

    modeCombo_ = new QComboBox();
    modeCombo_->addItems({"Engineer", "Operator"});

    auto* refreshButton = new QPushButton("Refresh");
    refreshButton->setIcon(themedIcon(this, "view-refresh", QStyle::SP_BrowserReload));
    emergencyStopButton_ = new QPushButton("Emergency Stop (Kill ROS)");
    emergencyStopButton_->setIcon(themedIcon(this, "process-stop", QStyle::SP_BrowserStop));
    emergencyStopButton_->setStyleSheet("background:#d64545;color:white;font-weight:700;");
    snapshotJsonButton_ = new QPushButton("Snapshot JSON");
    snapshotJsonButton_->setIcon(themedIcon(this, "document-save", QStyle::SP_DialogSaveButton));
    snapshotYamlButton_ = new QPushButton("Snapshot YAML");
    snapshotYamlButton_->setIcon(themedIcon(this, "document-save-as", QStyle::SP_DialogSaveButton));
    compareSnapshotButton_ = new QPushButton("Snapshot Diff");
    compareSnapshotButton_->setIcon(themedIcon(this, "view-sort-descending", QStyle::SP_ArrowDown));
    savePresetButton_ = new QPushButton("Save Preset");
    savePresetButton_->setIcon(themedIcon(this, "document-save", QStyle::SP_DialogSaveButton));
    loadPresetButton_ = new QPushButton("Load Preset");
    loadPresetButton_->setIcon(themedIcon(this, "document-open", QStyle::SP_DialogOpenButton));
    sessionStartButton_ = new QPushButton("Start Session");
    sessionStartButton_->setIcon(themedIcon(this, "media-playback-start", QStyle::SP_MediaPlay));
    sessionStopButton_ = new QPushButton("Stop Session");
    sessionStopButton_->setIcon(themedIcon(this, "media-playback-stop", QStyle::SP_MediaStop));
    sessionExportButton_ = new QPushButton("Export Session");
    sessionExportButton_->setIcon(themedIcon(this, "document-export", QStyle::SP_DialogSaveButton));
    telemetryExportButton_ = new QPushButton("Export Telemetry");
    telemetryExportButton_->setIcon(themedIcon(this, "document-export", QStyle::SP_DialogSaveButton));
    watchdogToggleButton_ = new QPushButton("Watchdog: OFF");
    watchdogToggleButton_->setIcon(themedIcon(this, "security-high", QStyle::SP_MessageBoxWarning));
    fleetRefreshButton_ = new QPushButton("Fleet Refresh");
    fleetRefreshButton_->setIcon(themedIcon(this, "view-refresh", QStyle::SP_BrowserReload));
    fleetLoadTargetsButton_ = new QPushButton("Load Fleet Targets");
    fleetLoadTargetsButton_->setIcon(themedIcon(this, "folder-open", QStyle::SP_DialogOpenButton));
    remoteRestartButton_ = new QPushButton("Remote Restart");
    remoteRestartButton_->setIcon(themedIcon(this, "system-reboot", QStyle::SP_BrowserReload));
    remoteKillButton_ = new QPushButton("Remote Kill");
    remoteKillButton_->setIcon(themedIcon(this, "process-stop", QStyle::SP_BrowserStop));
    healthLabel_ = new QLabel("Health: UNKNOWN");
    healthLabel_->setStyleSheet(
        "font-size:16px;font-weight:800;padding:6px 10px;border-radius:8px;background:#26303a;color:#dce8f5;");
    presetLabel_ = new QLabel("Preset: default");
    emergencyStopButton_->setMinimumHeight(34);

    auto* snapshotMenu = new QMenu(this);
    snapshotMenu->addAction(
        themedIcon(this, "document-save", QStyle::SP_DialogSaveButton),
        "JSON",
        [this]() { snapshotJsonButton_->click(); });
    snapshotMenu->addAction(
        themedIcon(this, "document-save-as", QStyle::SP_DialogSaveButton),
        "YAML",
        [this]() { snapshotYamlButton_->click(); });
    snapshotMenu->addAction(
        themedIcon(this, "view-sort-descending", QStyle::SP_ArrowDown),
        "Diff",
        [this]() { compareSnapshotButton_->click(); });
    auto* snapshotMenuButton = new QToolButton(this);
    snapshotMenuButton->setIcon(themedIcon(this, "document-save", QStyle::SP_DialogSaveButton));
    snapshotMenuButton->setText("Snapshot");
    snapshotMenuButton->setMenu(snapshotMenu);
    snapshotMenuButton->setPopupMode(QToolButton::InstantPopup);
    snapshotMenuButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    auto* sessionMenu = new QMenu(this);
    sessionMenu->addAction(
        themedIcon(this, "media-playback-start", QStyle::SP_MediaPlay),
        "Start",
        [this]() { sessionStartButton_->click(); });
    sessionMenu->addAction(
        themedIcon(this, "media-playback-stop", QStyle::SP_MediaStop),
        "Stop",
        [this]() { sessionStopButton_->click(); });
    sessionMenu->addAction(
        themedIcon(this, "document-export", QStyle::SP_DialogSaveButton),
        "Export",
        [this]() { sessionExportButton_->click(); });
    sessionMenu->addAction(
        themedIcon(this, "document-export", QStyle::SP_DialogSaveButton),
        "Export Telemetry",
        [this]() { telemetryExportButton_->click(); });
    auto* sessionMenuButton = new QToolButton(this);
    sessionMenuButton->setIcon(themedIcon(this, "view-calendar", QStyle::SP_FileDialogDetailedView));
    sessionMenuButton->setText("Session");
    sessionMenuButton->setMenu(sessionMenu);
    sessionMenuButton->setPopupMode(QToolButton::InstantPopup);
    sessionMenuButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    auto* presetMenu = new QMenu(this);
    presetMenu->addAction(
        themedIcon(this, "document-save", QStyle::SP_DialogSaveButton),
        "Save",
        [this]() { savePresetButton_->click(); });
    presetMenu->addAction(
        themedIcon(this, "document-open", QStyle::SP_DialogOpenButton),
        "Load",
        [this]() { loadPresetButton_->click(); });
    auto* presetMenuButton = new QToolButton(this);
    presetMenuButton->setIcon(themedIcon(this, "document-properties", QStyle::SP_FileDialogInfoView));
    presetMenuButton->setText("Preset");
    presetMenuButton->setMenu(presetMenu);
    presetMenuButton->setPopupMode(QToolButton::InstantPopup);
    presetMenuButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    auto* fleetMenu = new QMenu(this);
    fleetMenu->addAction(
        themedIcon(this, "folder-open", QStyle::SP_DialogOpenButton),
        "Load Targets",
        [this]() { fleetLoadTargetsButton_->click(); });
    fleetMenu->addAction(
        themedIcon(this, "view-refresh", QStyle::SP_BrowserReload),
        "Refresh",
        [this]() { fleetRefreshButton_->click(); });
    fleetMenu->addAction(
        themedIcon(this, "system-reboot", QStyle::SP_BrowserReload),
        "Remote Restart",
        [this]() { remoteRestartButton_->click(); });
    fleetMenu->addAction(
        themedIcon(this, "process-stop", QStyle::SP_BrowserStop),
        "Remote Kill",
        [this]() { remoteKillButton_->click(); });
    auto* fleetMenuButton = new QToolButton(this);
    fleetMenuButton->setIcon(themedIcon(this, "network-workgroup", QStyle::SP_DirIcon));
    fleetMenuButton->setText("Fleet");
    fleetMenuButton->setMenu(fleetMenu);
    fleetMenuButton->setPopupMode(QToolButton::InstantPopup);
    fleetMenuButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

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
    processPrevButton_->setIcon(themedIcon(this, "go-previous", QStyle::SP_ArrowBack));
    processNextButton_->setIcon(themedIcon(this, "go-next", QStyle::SP_ArrowForward));
    processPageLabel_ = new QLabel("Rows 0-0 / 0");
    terminateButton_ = new QPushButton("SIGTERM");
    forceKillButton_ = new QPushButton("SIGKILL");
    killTreeButton_ = new QPushButton("Kill Tree");
    terminateButton_->setIcon(themedIcon(this, "media-playback-stop", QStyle::SP_MediaStop));
    forceKillButton_->setIcon(themedIcon(this, "process-stop", QStyle::SP_BrowserStop));
    killTreeButton_->setIcon(themedIcon(this, "edit-delete", QStyle::SP_TrashIcon));
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
    processTable_->setAlternatingRowColors(true);
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
    domainTable_->setAlternatingRowColors(true);
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
    domainNodeTable_->setAlternatingRowColors(true);
    domainNodeTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    domainNodeTable_->horizontalHeader()->setStretchLastSection(true);
    domainLayout->addWidget(domainNodeTable_, 1);

    auto* domainControls = new QHBoxLayout();
    restartDomainButton_ = new QPushButton("Restart Domain");
    isolateDomainButton_ = new QPushButton("Isolate Domain");
    clearShmButton_ = new QPushButton("Clear Shared Memory");
    restartDomainButton_->setIcon(themedIcon(this, "system-reboot", QStyle::SP_BrowserReload));
    isolateDomainButton_->setIcon(themedIcon(this, "network-disconnect", QStyle::SP_DialogCancelButton));
    clearShmButton_->setIcon(themedIcon(this, "edit-clear", QStyle::SP_DialogResetButton));
    workspacePathInput_ = new QLineEdit();
    workspacePathInput_->setPlaceholderText("Workspace path (e.g. /home/user/ws/install)");
    workspaceRelaunchInput_ = new QLineEdit();
    workspaceRelaunchInput_->setPlaceholderText("Optional relaunch command");
    restartWorkspaceButton_ = new QPushButton("Restart Workspace");
    restartWorkspaceButton_->setIcon(themedIcon(this, "system-reboot", QStyle::SP_BrowserReload));
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
    fetchParamsButton_->setIcon(themedIcon(this, "view-refresh", QStyle::SP_BrowserReload));
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
    tfTable_->setAlternatingRowColors(true);
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

    auto* graphBox = new QWidget();
    auto* graphLayout = new QVBoxLayout(graphBox);
    cpuGraphLabel_ = new QLabel("CPU graph: (no data)");
    memGraphLabel_ = new QLabel("MEM graph: (no data)");
    diskGraphLabel_ = new QLabel("DISK graph: (no data)");
    netGraphLabel_ = new QLabel("NET graph: (no data)");
    cpuGraphLabel_->setStyleSheet("font-family:monospace;");
    memGraphLabel_->setStyleSheet("font-family:monospace;");
    diskGraphLabel_->setStyleSheet("font-family:monospace;");
    netGraphLabel_->setStyleSheet("font-family:monospace;");
    graphLayout->addWidget(cpuGraphLabel_);
    graphLayout->addWidget(memGraphLabel_);
    graphLayout->addWidget(diskGraphLabel_);
    graphLayout->addWidget(netGraphLabel_);
    systemLayout->addWidget(graphBox);

    htopPanel_ = new QPlainTextEdit();
    htopPanel_->setReadOnly(true);
    htopPanel_->setMaximumHeight(170);
    htopPanel_->setStyleSheet("font-family:monospace;");
    htopPanel_->setPlaceholderText("Runtime activity summary...");
    systemLayout->addWidget(htopPanel_);

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
    diagnosticsSummaryLabel_ = new QLabel("Diagnostics overview");
    diagnosticsSummaryLabel_->setStyleSheet("font-size:14px;font-weight:700;");
    diagnosticsTable_ = new QTableWidget();
    diagnosticsTable_->setColumnCount(2);
    diagnosticsTable_->setHorizontalHeaderLabels({"Check", "Result"});
    diagnosticsTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    diagnosticsTable_->setSelectionMode(QAbstractItemView::NoSelection);
    diagnosticsTable_->setAlternatingRowColors(true);
    diagnosticsTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    diagnosticsTable_->horizontalHeader()->setStretchLastSection(true);
    diagnosticsLayout->addWidget(diagnosticsSummaryLabel_);
    diagnosticsLayout->addWidget(diagnosticsTable_, 1);
    tabs_->addTab(diagnosticsTab, themedIcon(this, "utilities-system-monitor", QStyle::SP_ComputerIcon), "Diagnostics");

    auto* performanceTab = new QWidget();
    auto* performanceLayout = new QVBoxLayout(performanceTab);
    performanceSummaryLabel_ = new QLabel("Performance metrics");
    performanceSummaryLabel_->setStyleSheet("font-size:14px;font-weight:700;");
    performanceTable_ = new QTableWidget();
    performanceTable_->setColumnCount(2);
    performanceTable_->setHorizontalHeaderLabels({"Metric", "Value"});
    performanceTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    performanceTable_->setSelectionMode(QAbstractItemView::NoSelection);
    performanceTable_->setAlternatingRowColors(true);
    performanceTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    performanceTable_->horizontalHeader()->setStretchLastSection(true);
    performanceLayout->addWidget(performanceSummaryLabel_);
    performanceLayout->addWidget(performanceTable_, 1);
    tabs_->addTab(performanceTab, themedIcon(this, "office-chart-line", QStyle::SP_ArrowUp), "Performance");

    auto* safetyTab = new QWidget();
    auto* safetyLayout = new QVBoxLayout(safetyTab);
    safetySummaryLabel_ = new QLabel("Safety status");
    safetySummaryLabel_->setStyleSheet("font-size:14px;font-weight:700;");
    safetyTable_ = new QTableWidget();
    safetyTable_->setColumnCount(2);
    safetyTable_->setHorizontalHeaderLabels({"Signal", "State"});
    safetyTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    safetyTable_->setSelectionMode(QAbstractItemView::NoSelection);
    safetyTable_->setAlternatingRowColors(true);
    safetyTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    safetyTable_->horizontalHeader()->setStretchLastSection(true);
    safetyLayout->addWidget(safetySummaryLabel_);
    safetyLayout->addWidget(safetyTable_, 1);
    tabs_->addTab(safetyTab, themedIcon(this, "security-high", QStyle::SP_MessageBoxWarning), "Safety");

    auto* workspaceTab = new QWidget();
    auto* workspaceLayout = new QVBoxLayout(workspaceTab);
    workspaceSummaryLabel_ = new QLabel("Workspace health");
    workspaceSummaryLabel_->setStyleSheet("font-size:14px;font-weight:700;");
    workspaceTable_ = new QTableWidget();
    workspaceTable_->setColumnCount(2);
    workspaceTable_->setHorizontalHeaderLabels({"Item", "Details"});
    workspaceTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    workspaceTable_->setSelectionMode(QAbstractItemView::NoSelection);
    workspaceTable_->setAlternatingRowColors(true);
    workspaceTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    workspaceTable_->horizontalHeader()->setStretchLastSection(true);
    workspaceLayout->addWidget(workspaceSummaryLabel_);
    workspaceLayout->addWidget(workspaceTable_, 1);
    tabs_->addTab(workspaceTab, themedIcon(this, "folder-development", QStyle::SP_DirIcon), "Workspaces");

    auto* fleetTab = new QWidget();
    auto* fleetLayout = new QVBoxLayout(fleetTab);
    fleetSummaryLabel_ = new QLabel("Fleet status");
    fleetSummaryLabel_->setStyleSheet("font-size:14px;font-weight:700;");
    fleetTable_ = new QTableWidget();
    fleetTable_->setColumnCount(5);
    fleetTable_->setHorizontalHeaderLabels({"Target", "Reachability", "Nodes", "Load", "Mem Avail (KB)"});
    fleetTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    fleetTable_->setSelectionMode(QAbstractItemView::NoSelection);
    fleetTable_->setAlternatingRowColors(true);
    fleetTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    fleetTable_->horizontalHeader()->setStretchLastSection(true);
    fleetLayout->addWidget(fleetSummaryLabel_);
    fleetLayout->addWidget(fleetTable_, 1);
    tabs_->addTab(fleetTab, themedIcon(this, "network-workgroup", QStyle::SP_DirIcon), "Fleet");
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
            "About RosScope",
            "RosScope\n\n"
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
        if (!isAllProcessesScopeActive()) {
            scheduleRefresh(refreshIntervalMs_);
        }
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

        if (action == "compare_snapshots" || action == "compare_with_previous") {
            const QJsonObject summary = result.value("summary").toObject();
            if (diagnosticsSummaryLabel_ != nullptr && !summary.isEmpty()) {
                diagnosticsSummaryLabel_->setText(
                    QString("Diagnostics overview | Snapshot diff nodes +%1/-%2, topics +%3/-%4")
                        .arg(summary.value("nodes_added").toInt(0))
                        .arg(summary.value("nodes_removed").toInt(0))
                        .arg(summary.value("topics_added").toInt(0))
                        .arg(summary.value("topics_removed").toInt(0)));
            }
        }
        if ((action == "load_preset" || action == "save_preset") && success && presetLabel_ != nullptr) {
            presetLabel_->setText(
                QString("Preset: %1").arg(result.value("preset_name").toString("default")));
        }
        if ((action == "fleet_refresh" || action == "remote_action") && result.contains("fleet")) {
            cachedFleet_ = result.value("fleet").toObject();
            renderFleetPanel();
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
    const bool allProcessesScope = processScopeCombo_->currentText() == "All Processes";
    request.insert("process_scope", processScopeCombo_->currentText());
    request.insert("ros_only", processScopeCombo_->currentText() == "ROS Only");
    request.insert("process_query", processSearch_->text().trimmed());
    request.insert("process_offset", processOffset_);
    request.insert("process_limit", allProcessesScope ? qMin(processLimit_, 80) : processLimit_);
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
    if (!force && isAllProcessesScopeActive()) {
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

bool MainWindow::isAllProcessesScopeActive() const {
    return tabs_ != nullptr
        && tabs_->currentIndex() == 0
        && processScopeCombo_ != nullptr
        && processScopeCombo_->currentText() == "All Processes";
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
        const bool allProcessesScope =
            (processScopeCombo_ != nullptr && processScopeCombo_->currentText() == "All Processes");
        refreshIntervalMs_ = qMax(refreshIntervalMs_, allProcessesScope ? 5000 : 2200);
    }
    renderDiagnosticsPanel();
    renderPerformancePanel();
    renderSafetyPanel();
    renderWorkspacePanel();
    renderFleetPanel();
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
        QColor background = QColor("#243628");
        QColor foreground = QColor("#d9f4df");
        QString reason = "Healthy";
        if (!isRos) {
            background = QColor("#252b32");
            foreground = QColor("#90a0ae");
            reason = "Non-ROS process";
        } else if (zombieNodes.contains(nodeName)) {
            background = QColor("#432125");  // red
            foreground = QColor("#ffd6da");
            reason = "Zombie node: PID missing or invalid";
        } else if (qosMismatchNodes.contains(nodeName)) {
            background = QColor("#3f3823");  // yellow
            foreground = QColor("#ffefc7");
            reason = "QoS mismatch detected for one or more node topics";
        } else if (duplicateNodes.contains(nodeName)) {
            background = QColor("#3a2749");  // purple
            foreground = QColor("#ecd9ff");
            reason = "Duplicate node name detected";
        } else if (inactiveLifecycleNodes.contains(nodeName)) {
            background = QColor("#1f2e43");  // blue
            foreground = QColor("#d8e7ff");
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
            conflictItem->setForeground(QBrush(QColor("#ffd6da")));
            for (int c = 0; c < domainTable_->columnCount(); ++c) {
                QTableWidgetItem* item = domainTable_->item(row, c);
                if (item != nullptr) {
                    item->setBackground(QBrush(QColor("#432125")));
                    item->setForeground(QBrush(QColor("#ffd6da")));
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

    // Live sparkline graphs for key continuously-monitored values.
    const double cpuPct = cpu.value("usage_percent").toDouble();
    const double memPct = mem.value("used_percent").toDouble();
    const double diskPct = disk.value("used_percent").toDouble();
    appendHistory(&cpuHistory_, cpuPct);
    appendHistory(&memHistory_, memPct);
    appendHistory(&diskHistory_, diskPct);

    qint64 totalNetBytes = 0;
    for (const QJsonValue& value : cachedSystem_.value("network_interfaces").toArray()) {
        const QJsonObject iface = value.toObject();
        totalNetBytes += static_cast<qint64>(iface.value("rx_bytes").toDouble(0));
        totalNetBytes += static_cast<qint64>(iface.value("tx_bytes").toDouble(0));
    }
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    double netMbps = 0.0;
    if (previousNetSampleMs_ > 0 && nowMs > previousNetSampleMs_ && totalNetBytes >= previousNetBytes_) {
        const double dt = static_cast<double>(nowMs - previousNetSampleMs_) / 1000.0;
        if (dt > 0.0) {
            const double deltaBytes = static_cast<double>(totalNetBytes - previousNetBytes_);
            netMbps = (deltaBytes * 8.0) / 1000000.0 / dt;
        }
    }
    previousNetBytes_ = totalNetBytes;
    previousNetSampleMs_ = nowMs;
    appendHistory(&netHistory_, netMbps, 40);

    if (cpuGraphLabel_ != nullptr) {
        cpuGraphLabel_->setText(
            QString("CPU  %1%  [%2]").arg(cpuPct, 0, 'f', 1).arg(sparkline(cpuHistory_)));
    }
    if (memGraphLabel_ != nullptr) {
        memGraphLabel_->setText(
            QString("MEM  %1%  [%2]").arg(memPct, 0, 'f', 1).arg(sparkline(memHistory_)));
    }
    if (diskGraphLabel_ != nullptr) {
        diskGraphLabel_->setText(
            QString("DISK %1%  [%2]").arg(diskPct, 0, 'f', 1).arg(sparkline(diskHistory_)));
    }
    if (netGraphLabel_ != nullptr) {
        netGraphLabel_->setText(
            QString("NET  %1 Mbps [%2]").arg(netMbps, 0, 'f', 1).arg(sparkline(netHistory_, 20.0)));
    }

    if (htopPanel_ != nullptr) {
        const QString loadAvg = readLocalTextFile("/proc/loadavg").trimmed();
        int running = 0;
        int sleeping = 0;
        int other = 0;
        for (const QJsonValue& value : cachedProcessesVisible_) {
            const QString state = value.toObject().value("state").toString();
            if (state == "R") {
                running++;
            } else if (state == "S") {
                sleeping++;
            } else {
                other++;
            }
        }
        QStringList lines;
        lines << "System Activity";
        lines << QString("Tasks: %1 total | %2 running | %3 sleeping | %4 other")
                     .arg(processTotalFiltered_)
                     .arg(running)
                     .arg(sleeping)
                     .arg(other);
        lines << QString("CPU: %1% | Mem: %2% | Disk: %3%")
                     .arg(cpuPct, 0, 'f', 1)
                     .arg(memPct, 0, 'f', 1)
                     .arg(diskPct, 0, 'f', 1);
        lines << QString("Load Avg: %1").arg(loadAvg.isEmpty() ? "-" : loadAvg);
        lines << "Top visible by CPU:";
        const int topN = qMin(6, cachedProcessesVisible_.size());
        for (int i = 0; i < topN; ++i) {
            const QJsonObject p = cachedProcessesVisible_.at(i).toObject();
            lines << QString(" %1  %2%  %3 MB  %4")
                         .arg(p.value("pid").toInt())
                         .arg(p.value("cpu_percent").toDouble(), 0, 'f', 1)
                         .arg((p.value("memory_percent").toDouble() / 100.0)
                                  * (mem.value("total_kb").toDouble() / 1024.0),
                              0,
                              'f',
                              1)
                         .arg(p.value("name").toString());
        }
        htopPanel_->setPlainText(lines.join('\n'));
    }

    usbText_->setPlainText(joinArrayLines(cachedSystem_.value("usb_devices").toArray()));
    serialText_->setPlainText(joinArrayLines(cachedSystem_.value("serial_ports").toArray()));
    canText_->setPlainText(joinArrayLines(cachedSystem_.value("can_interfaces").toArray()));
    netText_->setPlainText(formatNetworkInterfaces(cachedSystem_.value("network_interfaces").toArray()));
}

void MainWindow::renderLogs() {
    logsText_->setPlainText(cachedLogs_);
}

void MainWindow::renderDiagnosticsPanel() {
    const QJsonObject rate = cachedAdvanced_.value("topic_rate_analyzer").toObject();
    const QJsonObject qos = cachedAdvanced_.value("qos_mismatch_detector").toObject();
    const QJsonObject lifecycle = cachedAdvanced_.value("lifecycle_timeline").toObject();
    const QJsonObject leaks = cachedAdvanced_.value("memory_leak_detection").toObject();
    const QJsonObject net = cachedAdvanced_.value("network_saturation_monitor").toObject();
    const QJsonObject launch = cachedAdvanced_.value("deterministic_launch_validation").toObject();
    const QJsonObject impact = cachedAdvanced_.value("dependency_impact_map").toObject();
    const QJsonArray impactNodes = impact.value("top_impact_nodes").toArray();

    QString topImpact = "none";
    if (!impactNodes.isEmpty()) {
        const QJsonObject row = impactNodes.first().toObject();
        topImpact = QString("%1 (downstream %2)")
                        .arg(row.value("node").toString("-"))
                        .arg(row.value("downstream_count").toInt(0));
    }

    QVector<QPair<QString, QString>> rows{
        {"Runtime Stability Score", QString::number(cachedAdvanced_.value("runtime_stability_score").toInt(0))},
        {"Topic Rate Issues", QString::number(rate.value("issue_count").toInt(rate.value("underperforming_publishers").toArray().size()))},
        {"QoS Mismatches", QString::number(qos.value("mismatch_count").toInt(0))},
        {"Lifecycle Stuck Nodes", QString::number(lifecycle.value("stuck_transitional_nodes").toArray().size())},
        {"Memory Leak Candidates", QString::number(leaks.value("candidate_count").toInt(0))},
        {"Congested Interfaces", QString::number(net.value("congested_interfaces").toArray().size())},
        {"Deterministic Launch", launch.value("valid").toBool(true) ? "Pass" : "Fail"},
        {"Top Dependency Impact", topImpact},
    };

    QSet<int> warningRows;
    QSet<int> criticalRows;
    if (rows.at(1).second.toInt() > 0) {
        warningRows.insert(1);
    }
    if (rows.at(2).second.toInt() > 0) {
        warningRows.insert(2);
    }
    if (rows.at(3).second.toInt() > 0) {
        warningRows.insert(3);
    }
    if (rows.at(4).second.toInt() > 0) {
        warningRows.insert(4);
    }
    if (rows.at(5).second.toInt() > 0) {
        warningRows.insert(5);
    }
    if (!launch.value("valid").toBool(true)) {
        criticalRows.insert(6);
    }

    populateKeyValueTable(diagnosticsTable_, rows, warningRows, criticalRows);
    if (diagnosticsSummaryLabel_ != nullptr) {
        diagnosticsSummaryLabel_->setText(
            QString("Diagnostics overview | score %1 | QoS mismatches %2 | leaks %3")
                .arg(rows.at(0).second)
                .arg(rows.at(2).second)
                .arg(rows.at(4).second));
    }
}

void MainWindow::renderPerformancePanel() {
    const QJsonObject topicRates = cachedAdvanced_.value("topic_rate_analyzer").toObject();
    const QJsonObject leaks = cachedAdvanced_.value("memory_leak_detection").toObject();
    const QJsonObject network = cachedAdvanced_.value("network_saturation_monitor").toObject();
    const QJsonObject correlation = cachedAdvanced_.value("cross_correlation_timeline").toObject();
    const QJsonObject cpu = cachedSystem_.value("cpu").toObject();
    const QJsonObject mem = cachedSystem_.value("memory").toObject();
    const QJsonObject disk = cachedSystem_.value("disk").toObject();

    const QJsonArray highTraffic = network.value("high_traffic_publishers").toArray();
    QString topTopic = "none";
    if (!highTraffic.isEmpty()) {
        const QJsonObject top = highTraffic.first().toObject();
        topTopic = QString("%1 (%2 Mbps)")
                       .arg(top.value("topic").toString("-"))
                       .arg(top.value("throughput_mbps").toDouble(), 0, 'f', 1);
    }

    QVector<QPair<QString, QString>> rows{
        {"CPU Usage", QString("%1%").arg(cpu.value("usage_percent").toDouble(), 0, 'f', 1)},
        {"Memory Usage", QString("%1%").arg(mem.value("used_percent").toDouble(), 0, 'f', 1)},
        {"Disk Usage", QString("%1%").arg(disk.value("used_percent").toDouble(), 0, 'f', 1)},
        {"Visible Processes", QString::number(cachedProcessesVisible_.size())},
        {"Filtered Processes", QString::number(processTotalFiltered_)},
        {"Topic Samples", QString::number(topicRates.value("topic_metrics").toArray().size())},
        {"Correlated Events", QString::number(correlation.value("correlated_events").toArray().size())},
        {"Leak Candidates", QString::number(leaks.value("candidate_count").toInt(0))},
        {"High Traffic Topics", QString::number(highTraffic.size())},
        {"Top High Traffic Topic", topTopic},
    };

    QSet<int> warningRows;
    if (cpu.value("usage_percent").toDouble() > 90.0) {
        warningRows.insert(0);
    }
    if (mem.value("used_percent").toDouble() > 90.0) {
        warningRows.insert(1);
    }
    if (disk.value("used_percent").toDouble() > 92.0) {
        warningRows.insert(2);
    }
    if (rows.at(7).second.toInt() > 0) {
        warningRows.insert(7);
    }
    if (rows.at(8).second.toInt() > 0) {
        warningRows.insert(8);
    }

    populateKeyValueTable(performanceTable_, rows, warningRows, {});
    if (performanceSummaryLabel_ != nullptr) {
        performanceSummaryLabel_->setText(
            QString("Performance metrics | CPU %1 | MEM %2 | active rows %3")
                .arg(rows.at(0).second)
                .arg(rows.at(1).second)
                .arg(rows.at(3).second));
    }
}

void MainWindow::renderSafetyPanel() {
    const QJsonObject soft = cachedAdvanced_.value("soft_safety_boundary").toObject();
    const QJsonObject tfDrift = cachedAdvanced_.value("tf_drift_monitor").toObject();
    const QString healthState = cachedHealth_.value("status").toString("unknown").toUpper();
    const int zombieCount = cachedHealth_.value("zombie_nodes").toArray().size();
    const int conflictCount = cachedHealth_.value("domain_conflicts").toArray().size();
    const int softWarnings = soft.value("warning_count").toInt(0);
    const int tfDuplicates = tfDrift.value("duplicate_count").toInt(0);

    QVector<QPair<QString, QString>> rows{
        {"Watchdog Enabled", boolText(cachedWatchdog_.value("enabled").toBool(false))},
        {"Health State", healthState},
        {"Zombie Nodes", QString::number(zombieCount)},
        {"Domain Conflicts", QString::number(conflictCount)},
        {"Soft Boundary Warnings", QString::number(softWarnings)},
        {"TF Duplicate Children", QString::number(tfDuplicates)},
        {"Emergency Controls", "Ready"},
    };

    QSet<int> warningRows;
    QSet<int> criticalRows;
    if (healthState == "CRITICAL") {
        criticalRows.insert(1);
    } else if (healthState == "WARNING" || healthState == "DEGRADED") {
        warningRows.insert(1);
    }
    if (zombieCount > 0) {
        criticalRows.insert(2);
    }
    if (conflictCount > 0) {
        warningRows.insert(3);
    }
    if (softWarnings > 0) {
        warningRows.insert(4);
    }
    if (tfDuplicates > 0) {
        warningRows.insert(5);
    }

    populateKeyValueTable(safetyTable_, rows, warningRows, criticalRows);
    if (safetySummaryLabel_ != nullptr) {
        safetySummaryLabel_->setText(
            QString("Safety status | %1 | zombies %2 | warnings %3")
                .arg(healthState)
                .arg(zombieCount)
                .arg(softWarnings + tfDuplicates));
    }
}

void MainWindow::renderWorkspacePanel() {
    const QJsonObject ws = cachedAdvanced_.value("workspace_tools").toObject();
    const QJsonArray chain = ws.value("overlay_chain").toArray();
    const QJsonArray dup = ws.value("duplicate_packages").toArray();
    const QJsonArray distros = ws.value("detected_distributions").toArray();
    const QJsonArray paramChanges =
        cachedAdvanced_.value("parameter_drift").toObject().value("changed_nodes").toArray();

    QStringList distroList;
    for (const QJsonValue& value : distros) {
        distroList.append(value.toString());
    }
    QStringList chainList;
    for (const QJsonValue& value : chain) {
        chainList.append(value.toString());
    }
    const QString distroText = distroList.isEmpty() ? "none" : distroList.join(", ");
    const QString chainPreview = chainList.isEmpty() ? "none" : chainList.mid(0, 4).join(" -> ");
    const QString chainSuffix = chainList.size() > 4 ? " -> ..." : "";

    QString duplicatePreview = "none";
    if (!dup.isEmpty()) {
        QStringList entries;
        const int limit = qMin(3, dup.size());
        for (int i = 0; i < limit; ++i) {
            const QJsonObject row = dup.at(i).toObject();
            entries.append(
                QString("%1 (%2)")
                    .arg(row.value("package").toString("-"))
                    .arg(row.value("workspaces").toArray().size()));
        }
        duplicatePreview = entries.join(", ");
    }

    QVector<QPair<QString, QString>> rows{
        {"Overlay Count", QString::number(chain.size())},
        {"Duplicate Packages", QString::number(dup.size())},
        {"Mixed ROS Distributions", boolText(ws.value("mixed_ros_distributions").toBool(false))},
        {"ABI Mismatch Suspected", boolText(ws.value("abi_mismatch_suspected").toBool(false))},
        {"Detected Distributions", distroText},
        {"Overlay Chain", chainPreview + chainSuffix},
        {"Duplicate Package Preview", duplicatePreview},
        {"Parameter Drift Nodes", QString::number(paramChanges.size())},
    };

    QSet<int> warningRows;
    if (rows.at(1).second.toInt() > 0) {
        warningRows.insert(1);
    }
    if (rows.at(2).second == "Yes") {
        warningRows.insert(2);
    }
    if (rows.at(3).second == "Yes") {
        warningRows.insert(3);
    }
    if (rows.at(7).second.toInt() > 0) {
        warningRows.insert(7);
    }

    populateKeyValueTable(workspaceTable_, rows, warningRows, {});
    if (workspaceSummaryLabel_ != nullptr) {
        workspaceSummaryLabel_->setText(
            QString("Workspace health | overlays %1 | duplicates %2 | distros %3")
                .arg(rows.at(0).second)
                .arg(rows.at(1).second)
                .arg(distros.size()));
    }
}

void MainWindow::renderFleetPanel() {
    if (fleetTable_ == nullptr) {
        return;
    }

    const QJsonArray robots = cachedFleet_.value("robots").toArray();
    if (robots.isEmpty()) {
        fleetTable_->clearContents();
        fleetTable_->setRowCount(1);
        fleetTable_->setItem(0, 0, new QTableWidgetItem("No fleet targets loaded"));
        for (int col = 1; col < fleetTable_->columnCount(); ++col) {
            fleetTable_->setItem(0, col, new QTableWidgetItem("-"));
        }
        if (fleetSummaryLabel_ != nullptr) {
            fleetSummaryLabel_->setText("Fleet status | load targets to monitor remote hosts");
        }
        return;
    }

    fleetTable_->clearContents();
    fleetTable_->setRowCount(robots.size());
    int row = 0;
    for (const QJsonValue& value : robots) {
        const QJsonObject robot = value.toObject();
        const QString name = robot.value("name").toString(robot.value("host").toString("unknown"));
        const bool reachable = robot.value("reachable").toBool(false);
        const QString reachability = reachable ? "Reachable" : "Unreachable";
        const QString nodeCount = robot.contains("node_count") ? QString::number(robot.value("node_count").toInt()) : "-";
        const QString load = robot.contains("load_1m")
            ? QString::number(robot.value("load_1m").toDouble(), 'f', 2)
            : "-";
        const QString mem = robot.contains("mem_available_kb")
            ? QString::number(static_cast<qint64>(robot.value("mem_available_kb").toDouble()))
            : "-";

        fleetTable_->setItem(row, 0, new QTableWidgetItem(name));
        fleetTable_->setItem(row, 1, new QTableWidgetItem(reachability));
        fleetTable_->setItem(row, 2, new QTableWidgetItem(nodeCount));
        fleetTable_->setItem(row, 3, new QTableWidgetItem(load));
        fleetTable_->setItem(row, 4, new QTableWidgetItem(mem));

        const QColor bg = reachable ? QColor("#23382a") : QColor("#432125");
        const QColor fg = reachable ? QColor("#d7f0de") : QColor("#ffd6da");
        const QString error = robot.value("error").toString();
        for (int c = 0; c < fleetTable_->columnCount(); ++c) {
            QTableWidgetItem* item = fleetTable_->item(row, c);
            if (item == nullptr) {
                continue;
            }
            item->setBackground(QBrush(bg));
            item->setForeground(QBrush(fg));
            if (!error.isEmpty()) {
                item->setToolTip(error);
            }
        }
        row++;
    }

    if (fleetSummaryLabel_ != nullptr) {
        fleetSummaryLabel_->setText(
            QString("Fleet status | healthy %1/%2 | offline queue %3")
                .arg(cachedFleet_.value("healthy_count").toInt(0))
                .arg(cachedFleet_.value("total_count").toInt(0))
                .arg(cachedFleet_.value("offline_queue_size").toInt(0)));
    }
}

void MainWindow::renderHealthSummary() {
    const QString status = cachedHealth_.value("status").toString("unknown").toLower();
    if (status == "critical") {
        healthLabel_->setStyleSheet(
            "font-size:18px;font-weight:800;padding:8px 12px;border-radius:10px;background:#4a252a;color:#ffd6da;");
    } else if (status == "warning") {
        healthLabel_->setStyleSheet(
            "font-size:18px;font-weight:800;padding:8px 12px;border-radius:10px;background:#4a3e20;color:#ffefc0;");
    } else {
        healthLabel_->setStyleSheet(
            "font-size:18px;font-weight:800;padding:8px 12px;border-radius:10px;background:#23412a;color:#d7f3dd;");
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
