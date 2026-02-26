#pragma once

#include <QCheckBox>
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
    QJsonObject buildPollRequest() const;
    QString selectedDomainId() const;

    void renderFromSnapshot(const QJsonObject& snapshot);
    void renderProcesses();
    void renderDomains();
    void renderNodesTopics();
    void renderTfNav2();
    void renderSystemHardware();
    void renderLogs();
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
    QString cachedLogs_;
    QString currentDomain_;

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
    QPushButton* watchdogToggleButton_ = nullptr;
    QPushButton* fleetRefreshButton_ = nullptr;
    QPushButton* fleetLoadTargetsButton_ = nullptr;
    QPushButton* remoteRestartButton_ = nullptr;
    QPushButton* remoteKillButton_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QTimer* refreshTimer_ = nullptr;

    QLineEdit* processSearch_ = nullptr;
    QCheckBox* rosOnlyCheck_ = nullptr;
    QTableWidget* processTable_ = nullptr;
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
    QPlainTextEdit* usbText_ = nullptr;
    QPlainTextEdit* serialText_ = nullptr;
    QPlainTextEdit* canText_ = nullptr;
    QPlainTextEdit* netText_ = nullptr;

    QPlainTextEdit* logsText_ = nullptr;

    QPlainTextEdit* diagnosticsText_ = nullptr;
    QPlainTextEdit* performanceText_ = nullptr;
    QPlainTextEdit* safetyText_ = nullptr;
    QPlainTextEdit* workspaceText_ = nullptr;
    QPlainTextEdit* fleetText_ = nullptr;
};

}  // namespace rrcc
