#pragma once

#include <QComboBox>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTabWidget>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QHash>

#include "rrcc/runtime_worker.hpp"

namespace rrcc {

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    MainWindow();
    ~MainWindow() override;

signals:
    void pollRequested(const QJsonObject& request);
    void actionRequested(const QString& action, const QJsonObject& payload);
    void nodeParametersRequested(const QString& domainId, const QString& nodeName);

private:
    void setupUi();
    void setupWorker();
    void setupConnections();
    void applyMode();

    void queueRefresh();
    void scheduleRefresh(int delayMs = 0, bool force = false);
    QJsonObject buildPollRequest() const;
    QString selectedDomainId() const;
    static qint64 processMemoryRssKb();
    void updateProcessPaginationLabel();
    void pruneNodeParameterCache();
    void updateProcessScopeOptions();
    void applyProcessTableMode();
    bool isAllProcessesScopeActive() const;

    void renderFromSnapshot(const QJsonObject& snapshot);
    void renderProcesses();
    void renderDomains();
    void renderNodesTopics();
    void renderTfNav2();
    void renderSystemHardware();
    void renderLogs();
    void renderDiagnosticsPanel();
    void renderPerformancePanel();
    void renderSafetyPanel();
    void renderWorkspacePanel();
    void renderFleetPanel();
    void renderHealthSummary();

    void runProcessAction(const QString& action);
    void runGlobalAction(const QString& action, const QJsonObject& payload = {});
    void showMessage(const QString& message, bool error = false) const;

    QThread* workerThread_ = nullptr;
    RuntimeWorker* worker_ = nullptr;

    QJsonArray cachedProcessesAll_;
    QJsonArray cachedProcessesVisible_;
    QJsonArray cachedDomainSummaries_;
    QJsonArray cachedDomains_;
    QJsonObject cachedGraph_;
    QJsonObject cachedTfNav2_;
    QJsonObject cachedSystem_;
    QJsonObject cachedHealth_;
    QJsonObject cachedAdvanced_;
    QJsonObject cachedFleet_;
    QJsonObject cachedSession_;
    QJsonObject cachedWatchdog_;
    QJsonObject cachedNodeParameters_;
    qint64 cachedSyncVersion_ = -1;
    QString cachedEtag_;
    int processOffset_ = 0;
    int processLimit_ = 400;
    int processTotalFiltered_ = 0;
    QStringList nodeParameterOrder_;
    int maxNodeParameterCache_ = 500;
    QString cachedLogs_;
    QString currentDomain_;
    QString lastProcessRenderHash_;
    QString lastDomainRenderHash_;

    QWidget* central_ = nullptr;
    QComboBox* modeCombo_ = nullptr;
    QLabel* healthLabel_ = nullptr;
    QLabel* presetLabel_ = nullptr;
    QPushButton* emergencyStopButton_ = nullptr;
    QPushButton* snapshotJsonButton_ = nullptr;
    QPushButton* snapshotYamlButton_ = nullptr;
    QPushButton* compareSnapshotButton_ = nullptr;
    QPushButton* savePresetButton_ = nullptr;
    QPushButton* loadPresetButton_ = nullptr;
    QPushButton* sessionStartButton_ = nullptr;
    QPushButton* sessionStopButton_ = nullptr;
    QPushButton* sessionExportButton_ = nullptr;
    QPushButton* telemetryExportButton_ = nullptr;
    QPushButton* watchdogToggleButton_ = nullptr;
    QPushButton* fleetRefreshButton_ = nullptr;
    QPushButton* fleetLoadTargetsButton_ = nullptr;
    QPushButton* remoteRestartButton_ = nullptr;
    QPushButton* remoteKillButton_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QTimer* refreshTimer_ = nullptr;
    QTimer* refreshDebounceTimer_ = nullptr;
    QTimer* eventLoopLagTimer_ = nullptr;
    QTimer* memoryWatchTimer_ = nullptr;
    bool refreshInFlight_ = false;
    int refreshIntervalMs_ = 1500;
    int minRefreshIntervalMs_ = 500;
    int maxRefreshIntervalMs_ = 12000;
    qint64 lastLagSampleEpochMs_ = 0;

    QLineEdit* processSearch_ = nullptr;
    QComboBox* processScopeCombo_ = nullptr;
    QComboBox* processViewModeCombo_ = nullptr;
    QTableWidget* processTable_ = nullptr;
    QPushButton* processPrevButton_ = nullptr;
    QPushButton* processNextButton_ = nullptr;
    QLabel* processPageLabel_ = nullptr;
    QPushButton* terminateButton_ = nullptr;
    QPushButton* forceKillButton_ = nullptr;
    QPushButton* killTreeButton_ = nullptr;

    QTableWidget* domainTable_ = nullptr;
    QTableWidget* domainNodeTable_ = nullptr;
    QPushButton* restartDomainButton_ = nullptr;
    QPushButton* isolateDomainButton_ = nullptr;
    QLineEdit* workspacePathInput_ = nullptr;
    QLineEdit* workspaceRelaunchInput_ = nullptr;
    QPushButton* restartWorkspaceButton_ = nullptr;
    QPushButton* clearShmButton_ = nullptr;

    QTreeWidget* nodesTree_ = nullptr;
    QPlainTextEdit* qosText_ = nullptr;
    QPlainTextEdit* paramsText_ = nullptr;
    QPushButton* fetchParamsButton_ = nullptr;

    QTableWidget* tfTable_ = nullptr;
    QPlainTextEdit* nav2Text_ = nullptr;

    QLabel* cpuLabel_ = nullptr;
    QLabel* memLabel_ = nullptr;
    QLabel* diskLabel_ = nullptr;
    QLabel* gpuLabel_ = nullptr;
    QLabel* cpuGraphLabel_ = nullptr;
    QLabel* memGraphLabel_ = nullptr;
    QLabel* diskGraphLabel_ = nullptr;
    QLabel* netGraphLabel_ = nullptr;
    QPlainTextEdit* htopPanel_ = nullptr;
    QVector<double> cpuHistory_;
    QVector<double> memHistory_;
    QVector<double> diskHistory_;
    QVector<double> netHistory_;
    qint64 previousNetBytes_ = 0;
    qint64 previousNetSampleMs_ = 0;
    QPlainTextEdit* usbText_ = nullptr;
    QPlainTextEdit* serialText_ = nullptr;
    QPlainTextEdit* canText_ = nullptr;
    QPlainTextEdit* netText_ = nullptr;

    QPlainTextEdit* logsText_ = nullptr;

    QLabel* diagnosticsSummaryLabel_ = nullptr;
    QTableWidget* diagnosticsTable_ = nullptr;
    QLabel* performanceSummaryLabel_ = nullptr;
    QTableWidget* performanceTable_ = nullptr;
    QLabel* safetySummaryLabel_ = nullptr;
    QTableWidget* safetyTable_ = nullptr;
    QLabel* workspaceSummaryLabel_ = nullptr;
    QTableWidget* workspaceTable_ = nullptr;
    QLabel* fleetSummaryLabel_ = nullptr;
    QTableWidget* fleetTable_ = nullptr;
};

}  // namespace rrcc
