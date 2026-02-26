#include "rrcc/remote_monitor.hpp"

#include <QFile>
#include <QJsonDocument>
#include <QStringList>

#include "rrcc/command_runner.hpp"

namespace rrcc {

QJsonObject RemoteMonitor::toJson(const Target& target) {
    return {
        {"name", target.name},
        {"host", target.host},
        {"user", target.user},
        {"port", target.port},
        {"domain_id", target.domainId},
        {"ros_setup", target.rosSetup},
    };
}

RemoteMonitor::Target RemoteMonitor::fromJson(const QJsonObject& object) {
    Target target;
    target.name = object.value("name").toString();
    target.host = object.value("host").toString();
    target.user = object.value("user").toString();
    target.port = object.value("port").toInt(22);
    target.domainId = object.value("domain_id").toString("0");
    target.rosSetup = object.value("ros_setup").toString("/opt/ros/humble/setup.bash");
    return target;
}

QString RemoteMonitor::hostKey(const Target& target) {
    const QString userPrefix = target.user.isEmpty() ? QString() : target.user + "@";
    return userPrefix + target.host;
}

QJsonObject RemoteMonitor::loadTargetsFromFile(const QString& filePath) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {
            {"success", false},
            {"error", "Failed to open remote targets file."},
            {"path", filePath},
        };
    }

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isArray()) {
        return {
            {"success", false},
            {"error", "Remote targets file must contain a JSON array."},
            {"path", filePath},
        };
    }

    targets_ = doc.array();
    return {
        {"success", true},
        {"loaded_targets", targets_.size()},
        {"path", filePath},
    };
}

void RemoteMonitor::setTargets(const QJsonArray& targets) {
    targets_ = targets;
}

QJsonArray RemoteMonitor::targets() const {
    return targets_;
}

QJsonObject RemoteMonitor::collectFleetStatus(int timeoutMs) const {
    QJsonArray robots;
    for (const QJsonValue& value : targets_) {
        const Target target = fromJson(value.toObject());
        if (target.host.isEmpty()) {
            continue;
        }

        const QString remoteScript = QString(
            "source %1 >/dev/null 2>&1; "
            "nodes=$(ros2 node list 2>/dev/null | wc -l); "
            "load=$(awk '{print $1}' /proc/loadavg); "
            "mem=$(awk '/MemAvailable/ {print $2}' /proc/meminfo); "
            "host=$(hostname); "
            "echo \"$host|$nodes|$load|$mem\"")
                                         .arg(target.rosSetup);

        const QStringList args = {
            "-o", "BatchMode=yes",
            "-o", "ConnectTimeout=3",
            "-p", QString::number(target.port),
            hostKey(target),
            "bash", "-lc", remoteScript,
        };
        const CommandResult result = CommandRunner::run("ssh", args, timeoutMs);

        QJsonObject robot = toJson(target);
        robot.insert("reachable", result.success());
        if (result.success()) {
            const QStringList parts = result.stdoutText.trimmed().split('|');
            if (parts.size() >= 4) {
                robot.insert("remote_hostname", parts[0]);
                robot.insert("node_count", parts[1].toInt());
                robot.insert("load_1m", parts[2].toDouble());
                robot.insert("mem_available_kb", parts[3].toLongLong());
            }
        } else {
            robot.insert("error", result.stderrText.trimmed());
        }
        robots.append(robot);
    }

    int healthy = 0;
    for (const QJsonValue& value : robots) {
        if (value.toObject().value("reachable").toBool(false)) {
            healthy++;
        }
    }

    return {
        {"robots", robots},
        {"healthy_count", healthy},
        {"total_count", robots.size()},
    };
}

QJsonObject RemoteMonitor::executeRemoteAction(
    const QString& targetName,
    const QString& action,
    const QString& domainId,
    int timeoutMs) const {
    for (const QJsonValue& value : targets_) {
        const Target target = fromJson(value.toObject());
        if (target.name != targetName) {
            continue;
        }

        QString remoteScript;
        if (action == "restart_domain") {
            remoteScript = QString(
                "source %1 >/dev/null 2>&1; export ROS_DOMAIN_ID=%2; ros2 daemon stop; ros2 daemon start;")
                               .arg(target.rosSetup, domainId);
        } else if (action == "kill_ros") {
            remoteScript = "pkill -9 -f -- '--ros-args|rclcpp|rclpy|/opt/ros|ament' || true";
        } else if (action == "isolate_domain") {
            remoteScript = QString(
                "source %1 >/dev/null 2>&1; export ROS_DOMAIN_ID=%2; ros2 daemon stop;")
                               .arg(target.rosSetup, domainId);
        } else {
            return {
                {"success", false},
                {"error", "Unsupported remote action."},
                {"target", targetName},
            };
        }

        const QStringList args = {
            "-o", "BatchMode=yes",
            "-o", "ConnectTimeout=3",
            "-p", QString::number(target.port),
            hostKey(target),
            "bash", "-lc", remoteScript,
        };
        const CommandResult result = CommandRunner::run("ssh", args, timeoutMs);
        return {
            {"success", result.success()},
            {"target", targetName},
            {"action", action},
            {"stderr", result.stderrText.trimmed()},
        };
    }

    return {
        {"success", false},
        {"error", "Remote target not found."},
        {"target", targetName},
    };
}

}  // namespace rrcc
