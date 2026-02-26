#include "rrcc/control_actions.hpp"

#include <QJsonArray>
#include <QJsonValue>

#include "rrcc/command_runner.hpp"
#include "rrcc/process_manager.hpp"

namespace rrcc {

ControlActions::ControlActions(ProcessManager* processManager)
    : processManager_(processManager) {}

namespace {
qint64 jsonPid(const QJsonObject& proc) {
    const QJsonValue value = proc.value("pid");
    if (value.isDouble()) {
        return static_cast<qint64>(value.toDouble(-1));
    }
    if (value.isString()) {
        return value.toString().toLongLong();
    }
    return -1;
}
}  // namespace

QJsonObject ControlActions::killAllRosProcesses(const QJsonArray& processes) const {
    int killed = 0;
    int failed = 0;
    for (const QJsonValue& value : processes) {
        const QJsonObject proc = value.toObject();
        if (!proc.value("is_ros").toBool()) {
            continue;
        }
        const qint64 pid = jsonPid(proc);
        if (pid <= 0) {
            continue;
        }
        if (processManager_->killProcessTree(pid, true)) {
            killed++;
        } else {
            failed++;
        }
    }
    QJsonObject out;
    out.insert("action", "kill_all_ros_processes");
    out.insert("killed_count", killed);
    out.insert("failed_count", failed);
    out.insert("success", failed == 0);
    return out;
}

QJsonObject ControlActions::restartDomain(
    const QString& domainId,
    const QJsonArray& processes) const {
    int terminated = 0;
    int failed = 0;
    for (const QJsonValue& value : processes) {
        const QJsonObject proc = value.toObject();
        if (!proc.value("is_ros").toBool()) {
            continue;
        }
        if (proc.value("ros_domain_id").toString("0") != domainId) {
            continue;
        }
        const qint64 pid = jsonPid(proc);
        if (pid <= 0) {
            continue;
        }
        if (processManager_->killProcessTree(pid, true)) {
            terminated++;
        } else {
            failed++;
        }
    }

    const QMap<QString, QString> env = {{"ROS_DOMAIN_ID", domainId}};
    const CommandResult stopDaemon = CommandRunner::run("ros2", {"daemon", "stop"}, 3000, env);
    const CommandResult startDaemon = CommandRunner::run("ros2", {"daemon", "start"}, 3000, env);

    QJsonObject out;
    out.insert("action", "restart_domain");
    out.insert("domain_id", domainId);
    out.insert("terminated_processes", terminated);
    out.insert("failed_processes", failed);
    out.insert("daemon_stop_ok", stopDaemon.success());
    out.insert("daemon_start_ok", startDaemon.success());
    out.insert("success", failed == 0 && startDaemon.success());
    out.insert("details", stopDaemon.stderrText + "\n" + startDaemon.stderrText);
    return out;
}

QJsonObject ControlActions::clearSharedMemory() const {
    const CommandResult rmFastdds = CommandRunner::runShell(
        "rm -f /dev/shm/fastrtps* /dev/shm/fastdds* /dev/shm/cyclonedds* /dev/shm/iceoryx*");
    const CommandResult ipcsCleanup = CommandRunner::runShell(
        "ipcs -m | awk 'NR>3 {print $2}' | xargs -r -n1 ipcrm -m");

    QJsonObject out;
    out.insert("action", "clear_shared_memory");
    out.insert("filesystem_cleanup_ok", rmFastdds.success());
    out.insert("ipcs_cleanup_ok", ipcsCleanup.success());
    out.insert("success", rmFastdds.success() || ipcsCleanup.success());
    out.insert("details", rmFastdds.stderrText + "\n" + ipcsCleanup.stderrText);
    return out;
}

QJsonObject ControlActions::restartWorkspace(
    const QString& workspacePath,
    const QString& relaunchCommand,
    const QJsonArray& processes) const {
    if (workspacePath.trimmed().isEmpty()) {
        return {
            {"action", "restart_workspace"},
            {"workspace_path", workspacePath},
            {"success", false},
            {"error", "Workspace path is required."},
        };
    }

    int terminated = 0;
    int failed = 0;
    for (const QJsonValue& value : processes) {
        const QJsonObject proc = value.toObject();
        if (!proc.value("is_ros").toBool()) {
            continue;
        }
        if (!proc.value("workspace_origin").toString().contains(workspacePath)) {
            continue;
        }
        const qint64 pid = jsonPid(proc);
        if (pid <= 0) {
            continue;
        }
        if (processManager_->killProcessTree(pid, true)) {
            terminated++;
        } else {
            failed++;
        }
    }

    bool relaunched = false;
    QString relaunchOutput;
    if (!relaunchCommand.trimmed().isEmpty()) {
        const QString cmd = QString("source %1/setup.bash && %2")
                                .arg(workspacePath.trimmed())
                                .arg(relaunchCommand.trimmed());
        const CommandResult relaunch = CommandRunner::runShell(cmd, 4000);
        relaunched = relaunch.success();
        relaunchOutput = relaunch.stdoutText + "\n" + relaunch.stderrText;
    }

    QJsonObject out;
    out.insert("action", "restart_workspace");
    out.insert("workspace_path", workspacePath);
    out.insert("terminated_processes", terminated);
    out.insert("failed_processes", failed);
    out.insert("relaunched", relaunched);
    out.insert("relaunch_output", relaunchOutput);
    out.insert("success", failed == 0);
    return out;
}

}  // namespace rrcc
