#include "rrcc/remote_monitor.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QStringList>
#include <QThread>

#include "rrcc/command_runner.hpp"
#include "rrcc/telemetry.hpp"

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

QString RemoteMonitor::queuePath() const {
    return QDir(QDir::currentPath()).filePath("state/offline_remote_queue.json");
}

void RemoteMonitor::loadQueue() {
    QFile file(queuePath());
    if (!file.exists()) {
        offlineQueue_ = QJsonArray{};
        return;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    if (doc.isArray()) {
        offlineQueue_ = doc.array();
    }
}

void RemoteMonitor::persistQueue() const {
    const QString path = queuePath();
    QDir dir = QFileInfo(path).absoluteDir();
    if (!dir.exists()) {
        dir.mkpath(".");
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }
    file.write(QJsonDocument(offlineQueue_).toJson(QJsonDocument::Indented));
    file.close();
}

void RemoteMonitor::enqueueOfflineAction(const QJsonObject& action) {
    if (offlineQueue_.isEmpty()) {
        loadQueue();
    }
    offlineQueue_.append(action);
    while (offlineQueue_.size() > maxOfflineQueue_) {
        offlineQueue_.removeFirst();
    }
    Telemetry::instance().setQueueSize("offline_remote_actions", offlineQueue_.size());
    persistQueue();
}

bool RemoteMonitor::isCircuitOpen(const QString& key) const {
    if (!circuit_.contains(key)) {
        return false;
    }
    return circuit_.value(key).openUntilMs > QDateTime::currentMSecsSinceEpoch();
}

void RemoteMonitor::onCircuitSuccess(const QString& key) const {
    circuit_.remove(key);
}

void RemoteMonitor::onCircuitFailure(const QString& key) const {
    CircuitState st = circuit_.value(key);
    st.failures++;
    if (st.failures >= circuitFailureThreshold_) {
        st.openUntilMs = QDateTime::currentMSecsSinceEpoch() + circuitCooldownMs_;
        Telemetry::instance().recordEvent(
            "circuit_open",
            {
                {"key", key},
                {"cooldown_ms", circuitCooldownMs_},
            });
    }
    circuit_.insert(key, st);
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
    loadQueue();
    Telemetry::instance().setGauge("fleet.targets_count", targets_.size());
    Telemetry::instance().setQueueSize("offline_remote_actions", offlineQueue_.size());
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

        const QString key = target.name + "|status";
        QJsonObject robot = toJson(target);
        if (isCircuitOpen(key)) {
            robot.insert("reachable", false);
            robot.insert("error", "Circuit breaker open (cooldown).");
            robots.append(robot);
            Telemetry::instance().incrementCounter("fleet.status.circuit_open");
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

        CommandResult result;
        for (int attempt = 0; attempt < 2; ++attempt) {
            Telemetry::instance().recordRequest();
            result = CommandRunner::run("ssh", args, timeoutMs);
            if (result.success()) {
                onCircuitSuccess(key);
                break;
            }
            onCircuitFailure(key);
            Telemetry::instance().incrementCounter("fleet.status.retry_count");
            const int jitter = static_cast<int>(QRandomGenerator::global()->bounded(200));
            QThread::msleep(static_cast<unsigned long>(150 + jitter));
        }

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
        {"offline_queue_size", offlineQueue_.size()},
    };
}

QJsonObject RemoteMonitor::executeRemoteActionInternal(
    const QString& targetName,
    const QString& action,
    const QString& domainId,
    int timeoutMs,
    bool allowQueueWrite) {
    for (const QJsonValue& value : targets_) {
        const Target target = fromJson(value.toObject());
        if (target.name != targetName) {
            continue;
        }

        const QString circuitKey = target.name + "|" + action;
        if (isCircuitOpen(circuitKey)) {
            Telemetry::instance().incrementCounter("fleet.action.circuit_open");
            return {
                {"success", false},
                {"error", "Circuit breaker open; cooldown active."},
                {"target", targetName},
                {"action", action},
            };
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

        CommandResult result;
        int retriesUsed = 0;
        for (int attempt = 0; attempt < maxRetries_; ++attempt) {
            Telemetry::instance().recordRequest();
            result = CommandRunner::run("ssh", args, timeoutMs);
            if (result.success()) {
                onCircuitSuccess(circuitKey);
                break;
            }
            retriesUsed = attempt + 1;
            onCircuitFailure(circuitKey);
            Telemetry::instance().incrementCounter("fleet.action.retry_count");
            const int base = 250 * (1 << attempt);
            const int jitter = static_cast<int>(QRandomGenerator::global()->bounded(350));
            const int sleepMs = qMin(9000, base + jitter);
            QThread::msleep(static_cast<unsigned long>(sleepMs));
        }

        if (!result.success() && allowQueueWrite) {
            enqueueOfflineAction(
                {
                    {"target", targetName},
                    {"action", action},
                    {"domain_id", domainId},
                    {"queued_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                });
            Telemetry::instance().incrementCounter("fleet.action.offline_queued");
        }

        return {
            {"success", result.success()},
            {"target", targetName},
            {"action", action},
            {"retry_count", retriesUsed},
            {"stderr", result.stderrText.trimmed()},
            {"offline_queue_size", offlineQueue_.size()},
        };
    }

    return {
        {"success", false},
        {"error", "Remote target not found."},
        {"target", targetName},
    };
}

QJsonObject RemoteMonitor::executeRemoteAction(
    const QString& targetName,
    const QString& action,
    const QString& domainId,
    int timeoutMs) {
    loadQueue();
    return executeRemoteActionInternal(targetName, action, domainId, timeoutMs, true);
}

QJsonObject RemoteMonitor::resumeQueuedActions(int budget, int timeoutMs) {
    loadQueue();
    if (offlineQueue_.isEmpty() || budget <= 0) {
        return {
            {"success", true},
            {"resumed_count", 0},
            {"remaining_queue", offlineQueue_.size()},
        };
    }

    int resumed = 0;
    int failed = 0;
    int idx = 0;
    while (idx < offlineQueue_.size() && resumed < budget) {
        const QJsonObject req = offlineQueue_.at(idx).toObject();
        const QJsonObject result = executeRemoteActionInternal(
            req.value("target").toString(),
            req.value("action").toString(),
            req.value("domain_id").toString("0"),
            timeoutMs,
            false);
        if (result.value("success").toBool(false)) {
            offlineQueue_.removeAt(idx);
            resumed++;
            continue;
        }
        failed++;
        idx++;
    }

    persistQueue();
    Telemetry::instance().setQueueSize("offline_remote_actions", offlineQueue_.size());
    return {
        {"success", true},
        {"resumed_count", resumed},
        {"failed_count", failed},
        {"remaining_queue", offlineQueue_.size()},
    };
}

}  // namespace rrcc
