#pragma once

#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include "rrcc/control_actions.hpp"
#include "rrcc/diagnostics_engine.hpp"
#include "rrcc/health_monitor.hpp"
#include "rrcc/process_manager.hpp"
#include "rrcc/remote_monitor.hpp"
#include "rrcc/ros_inspector.hpp"
#include "rrcc/session_recorder.hpp"
#include "rrcc/snapshot_manager.hpp"
#include "rrcc/snapshot_diff.hpp"
#include "rrcc/system_monitor.hpp"

namespace rrcc {

// Background worker that polls ROS/system state and executes control actions.
class RuntimeWorker final : public QObject {
    Q_OBJECT

public:
    explicit RuntimeWorker(QObject* parent = nullptr);

public slots:
    void poll(const QJsonObject& request);
    void runAction(const QString& action, const QJsonObject& payload);
    void fetchNodeParameters(const QString& domainId, const QString& nodeName);

signals:
    void snapshotReady(const QJsonObject& snapshot);
    void actionFinished(const QJsonObject& result);
    void nodeParametersReady(const QJsonObject& result);

private:
    void pollNow();
    QJsonArray applyProcessFilter(
        const QJsonArray& processes,
        bool rosOnly,
        const QString& query) const;
    QJsonObject buildResponse(
        const QString& selectedDomain,
        const QJsonArray& allProcesses,
        const QJsonArray& visibleProcesses,
        const QJsonArray& domainSummaries,
        const QJsonArray& domainDetails,
        const QJsonObject& graph,
        const QJsonObject& tfNav2,
        const QJsonObject& system,
        const QString& logs,
        const QJsonObject& health,
        const QJsonObject& advanced,
        const QJsonObject& fleet,
        const QJsonObject& session,
        const QJsonObject& watchdog) const;
    void applyWatchdog(const QString& selectedDomain);
    QJsonObject saveRuntimePreset(const QString& name) const;
    QJsonObject loadRuntimePreset(const QString& name);

    ProcessManager processManager_;
    RosInspector rosInspector_;
    HealthMonitor healthMonitor_;
    SnapshotManager snapshotManager_;
    SnapshotDiff snapshotDiff_;
    SessionRecorder sessionRecorder_;
    DiagnosticsEngine diagnosticsEngine_;
    RemoteMonitor remoteMonitor_;
    SystemMonitor systemMonitor_;
    ControlActions actions_;

    QJsonObject request_;
    bool busy_ = false;
    bool pending_ = false;
    int pollCounter_ = 0;

    QJsonArray lastAllProcesses_;
    QJsonArray lastVisibleProcesses_;
    QJsonArray lastDomainSummaries_;
    QJsonArray lastDomainDetails_;
    QJsonObject lastGraph_;
    QJsonObject lastTfNav2_;
    QJsonObject lastSystem_;
    QString lastLogs_;
    QJsonObject lastHealth_;
    QJsonObject parameterCache_;
    QJsonObject lastAdvanced_;
    QJsonObject lastFleet_;
    QJsonObject lastWatchdog_;
    QJsonObject previousSnapshot_;
    QJsonObject penultimateSnapshot_;
    QString presetName_ = "default";
    bool watchdogEnabled_ = false;
    qint64 lastWatchdogActionMs_ = 0;
};

}  // namespace rrcc
