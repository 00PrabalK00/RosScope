#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace rrcc {

class ProcessManager;

class ControlActions {
public:
    explicit ControlActions(ProcessManager* processManager);

    QJsonObject killAllRosProcesses(const QJsonArray& processes) const;
    QJsonObject restartDomain(const QString& domainId, const QJsonArray& processes) const;
    QJsonObject clearSharedMemory() const;
    QJsonObject restartWorkspace(
        const QString& workspacePath,
        const QString& relaunchCommand,
        const QJsonArray& processes) const;

private:
    ProcessManager* processManager_;
};

}  // namespace rrcc

