#include "rrcc/process_manager.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QtGlobal>

#include <algorithm>
#include <csignal>

#ifdef __linux__
#include <unistd.h>
#endif

namespace {

constexpr const char* kRosHints[] = {
    "ros2",
    "rclcpp",
    "rclpy",
    "librclcpp",
    "librclpy",
    "libfastrtps",
    "libcyclonedds",
    "libdds",
};

QString firstPathEntry(const QString& value) {
    const QStringList entries = value.split(':', Qt::SkipEmptyParts);
    return entries.isEmpty() ? QString() : entries.first();
}

}  // namespace

namespace rrcc {

bool ProcessManager::isNumeric(const QString& value) {
    for (const QChar c : value) {
        if (!c.isDigit()) {
            return false;
        }
    }
    return !value.isEmpty();
}

QString ProcessManager::readFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

QJsonObject ProcessManager::readStatus(const QString& pidPath) {
    QJsonObject status;
    const QString content = readFile(pidPath + "/status");
    const QStringList lines = content.split('\n', Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        const int idx = line.indexOf(':');
        if (idx <= 0) {
            continue;
        }
        const QString key = line.left(idx).trimmed();
        const QString value = line.mid(idx + 1).trimmed();
        status.insert(key, value);
    }
    return status;
}

QMap<QString, QString> ProcessManager::readEnviron(const QString& pidPath) {
    QMap<QString, QString> env;
    QFile file(pidPath + "/environ");
    if (!file.open(QIODevice::ReadOnly)) {
        return env;
    }
    const QByteArray content = file.readAll();
    const QList<QByteArray> entries = content.split('\0');
    for (const QByteArray& entry : entries) {
        const int eq = entry.indexOf('=');
        if (eq <= 0) {
            continue;
        }
        const QString key = QString::fromUtf8(entry.left(eq));
        const QString value = QString::fromUtf8(entry.mid(eq + 1));
        env.insert(key, value);
    }
    return env;
}

QString ProcessManager::readCmdline(const QString& pidPath) {
    QFile file(pidPath + "/cmdline");
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QByteArray content = file.readAll();
    content.replace('\0', ' ');
    return QString::fromUtf8(content).trimmed();
}

QString ProcessManager::readExePath(const QString& pidPath) {
    QFileInfo info(pidPath + "/exe");
    return info.symLinkTarget();
}

QString ProcessManager::detectWorkspaceOrigin(
    const QString& exePath,
    const QMap<QString, QString>& env) {
    const QString ament = firstPathEntry(env.value("AMENT_PREFIX_PATH"));
    if (!ament.isEmpty()) {
        return ament;
    }

    const QString colcon = firstPathEntry(env.value("COLCON_PREFIX_PATH"));
    if (!colcon.isEmpty()) {
        return colcon;
    }

    if (exePath.startsWith("/opt/ros/")) {
        const QStringList parts = exePath.split('/', Qt::SkipEmptyParts);
        if (parts.size() >= 3) {
            return "/" + parts[0] + "/" + parts[1] + "/" + parts[2];
        }
    }

    const QRegularExpression installRegex("^(.*/install/[^/]+)");
    const QRegularExpressionMatch match = installRegex.match(exePath);
    if (match.hasMatch()) {
        return match.captured(1);
    }

    return {};
}

QString ProcessManager::detectPackage(const QString& exePath, const QString& cmdline) {
    const QRegularExpression installPkg("/install/([^/]+)/");
    QRegularExpressionMatch match = installPkg.match(exePath);
    if (match.hasMatch()) {
        return match.captured(1);
    }

    const QRegularExpression rosRunRegex("ros2\\s+run\\s+([^\\s]+)\\s+");
    match = rosRunRegex.match(cmdline);
    if (match.hasMatch()) {
        return match.captured(1);
    }

    return {};
}

QString ProcessManager::detectLaunchSource(const QString& cmdline) {
    const QStringList tokens = cmdline.split(' ', Qt::SkipEmptyParts);
    for (const QString& token : tokens) {
        if (token.contains(".launch.py") || token.contains(".launch.xml")
            || token.contains(".launch.yaml") || token.contains(".launch.yml")) {
            return token;
        }
    }
    return {};
}

QString ProcessManager::detectNodeName(const QString& cmdline) {
    const QRegularExpression nodeRegex("__node:=([^\\s]+)");
    const QRegularExpressionMatch match = nodeRegex.match(cmdline);
    if (match.hasMatch()) {
        return match.captured(1);
    }
    return {};
}

QString ProcessManager::detectNamespace(const QString& cmdline) {
    const QRegularExpression nsRegex("__ns:=([^\\s]+)");
    const QRegularExpressionMatch match = nsRegex.match(cmdline);
    if (match.hasMatch()) {
        return match.captured(1);
    }
    return "/";
}

bool ProcessManager::isRosProcess(
    const QString& pidPath,
    const QString& exePath,
    const QString& cmdline,
    const QMap<QString, QString>& env) {
    if (env.contains("ROS_DOMAIN_ID") || env.contains("ROS_VERSION")
        || env.contains("AMENT_PREFIX_PATH") || env.contains("COLCON_PREFIX_PATH")) {
        return true;
    }

    const QString lowerCmdline = cmdline.toLower();
    if (lowerCmdline.contains("--ros-args")
        || lowerCmdline.contains("__node:=")
        || lowerCmdline.contains("__ns:=")
        || lowerCmdline.contains("ros2 ")) {
        return true;
    }

    const QString haystack = (exePath + " " + lowerCmdline).toLower();
    for (const char* hint : kRosHints) {
        if (haystack.contains(QString::fromUtf8(hint))) {
            return true;
        }
    }

    const QString maps = readFile(pidPath + "/maps").toLower();
    if (maps.contains("librclcpp")
        || maps.contains("librclpy")
        || maps.contains("librmw")
        || maps.contains("libfastrtps")
        || maps.contains("libfastdds")
        || maps.contains("libcyclonedds")
        || maps.contains("libdds")) {
        return true;
    }
    return false;
}

double ProcessManager::memoryPercentKb(qulonglong vmRssKb, qulonglong memTotalKb) {
    if (memTotalKb == 0) {
        return 0.0;
    }
    return 100.0 * static_cast<double>(vmRssKb) / static_cast<double>(memTotalKb);
}

QString ProcessManager::uptimeString(double seconds) {
    if (seconds < 0.0) {
        return "0s";
    }
    const int sec = static_cast<int>(seconds);
    const int h = sec / 3600;
    const int m = (sec % 3600) / 60;
    const int s = sec % 60;
    if (h > 0) {
        return QString("%1h %2m %3s").arg(h).arg(m).arg(s);
    }
    if (m > 0) {
        return QString("%1m %2s").arg(m).arg(s);
    }
    return QString("%1s").arg(s);
}

qulonglong ProcessManager::totalJiffies() {
    const QString stat = readFile("/proc/stat");
    const QString firstLine = stat.split('\n').value(0);
    const QStringList fields = firstLine.split(' ', Qt::SkipEmptyParts);
    if (fields.size() < 8) {
        return 0;
    }
    qulonglong total = 0;
    for (int i = 1; i < fields.size(); ++i) {
        total += fields[i].toULongLong();
    }
    return total;
}

qulonglong ProcessManager::memoryTotalKb() {
    const QString meminfo = readFile("/proc/meminfo");
    const QStringList lines = meminfo.split('\n', Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        if (line.startsWith("MemTotal:")) {
            const QStringList parts = line.split(' ', Qt::SkipEmptyParts);
            if (parts.size() >= 2) {
                return parts[1].toULongLong();
            }
        }
    }
    return 0;
}

double ProcessManager::systemUptimeSeconds() {
    const QString uptime = readFile("/proc/uptime");
    const QString firstToken = uptime.split(' ', Qt::SkipEmptyParts).value(0);
    return firstToken.toDouble();
}

QList<qint64> ProcessManager::listChildren(qint64 parentPid) {
    QList<qint64> children;
    const QDir procDir("/proc");
    const QStringList entries = procDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& entry : entries) {
        if (!isNumeric(entry)) {
            continue;
        }
        const QString statLine = readFile("/proc/" + entry + "/stat");
        const int endParen = statLine.lastIndexOf(')');
        if (endParen < 0 || endParen + 2 >= statLine.size()) {
            continue;
        }
        const QStringList tokens =
            statLine.mid(endParen + 2).split(' ', Qt::SkipEmptyParts);
        if (tokens.size() < 2) {
            continue;
        }
        const qint64 ppid = tokens[1].toLongLong();
        if (ppid == parentPid) {
            children.append(entry.toLongLong());
        }
    }
    return children;
}

void ProcessManager::collectChildrenRecursive(qint64 pid, QSet<qint64>& outSet) {
    const QList<qint64> children = listChildren(pid);
    for (qint64 child : children) {
        if (outSet.contains(child)) {
            continue;
        }
        outSet.insert(child);
        collectChildrenRecursive(child, outSet);
    }
}

QJsonArray ProcessManager::listProcesses(bool rosOnly, const QString& query) {
#ifndef __linux__
    Q_UNUSED(rosOnly);
    Q_UNUSED(query);
    return {};
#else
    const qulonglong currentTotalJiffies = totalJiffies();
    const qulonglong memTotal = memoryTotalKb();
    const double uptimeSecs = systemUptimeSeconds();
    const long hz = sysconf(_SC_CLK_TCK);
    const int cpuCores = std::max(1, static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN)));
    const QString queryLower = query.trimmed().toLower();

    QHash<qint64, qulonglong> currentProcJiffies;
    QList<QJsonObject> rows;

    const QDir procDir("/proc");
    const QStringList entries = procDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& entry : entries) {
        if (!isNumeric(entry)) {
            continue;
        }

        const qint64 pid = entry.toLongLong();
        const QString pidPath = "/proc/" + entry;
        const QString statLine = readFile(pidPath + "/stat");
        if (statLine.isEmpty()) {
            continue;
        }

        const int leftParen = statLine.indexOf('(');
        const int rightParen = statLine.lastIndexOf(')');
        if (leftParen < 0 || rightParen < 0 || rightParen <= leftParen) {
            continue;
        }

        const QString procName = statLine.mid(leftParen + 1, rightParen - leftParen - 1);
        const QString after = statLine.mid(rightParen + 2).trimmed();
        const QStringList fields = after.split(' ', Qt::SkipEmptyParts);
        if (fields.size() < 20) {
            continue;
        }

        const qint64 ppid = fields[1].toLongLong();
        const qulonglong utime = fields[11].toULongLong();
        const qulonglong stime = fields[12].toULongLong();
        const int threadCount = fields[17].toInt();
        const qulonglong starttimeTicks = fields[19].toULongLong();
        const qulonglong procJiffies = utime + stime;

        currentProcJiffies.insert(pid, procJiffies);

        double cpuPercent = 0.0;
        const qulonglong deltaTotal = currentTotalJiffies - previousTotalJiffies_;
        if (!firstCpuSample_ && deltaTotal > 0 && previousProcJiffies_.contains(pid)) {
            const qulonglong deltaProc = procJiffies - previousProcJiffies_.value(pid);
            cpuPercent = (100.0 * static_cast<double>(deltaProc) * static_cast<double>(cpuCores))
                / static_cast<double>(deltaTotal);
            if (cpuPercent < 0.0) {
                cpuPercent = 0.0;
            }
        }

        const QJsonObject status = readStatus(pidPath);
        qulonglong vmRssKb = 0;
        const QString vmRss = status.value("VmRSS").toString();
        if (!vmRss.isEmpty()) {
            vmRssKb = vmRss.split(' ', Qt::SkipEmptyParts).value(0).toULongLong();
        }
        const double memPercent = memoryPercentKb(vmRssKb, memTotal);

        const QString cmdline = readCmdline(pidPath);
        const QString exePath = readExePath(pidPath);
        const QMap<QString, QString> env = readEnviron(pidPath);

        const QString domainId = env.value("ROS_DOMAIN_ID", "0");
        const bool ros = isRosProcess(pidPath, exePath, cmdline, env);

        if (rosOnly && !ros) {
            continue;
        }

        const QString nodeName = detectNodeName(cmdline);
        const QString nodeNs = detectNamespace(cmdline);
        const QString workspace = detectWorkspaceOrigin(exePath, env);
        const QString packageName = detectPackage(exePath, cmdline);
        const QString launchSource = detectLaunchSource(cmdline);

        if (!queryLower.isEmpty()) {
            const QString searchable =
                QString::number(pid) + " " + procName + " " + exePath + " " + cmdline;
            if (!searchable.toLower().contains(queryLower)) {
                continue;
            }
        }

        const double procUptime =
            uptimeSecs - (static_cast<double>(starttimeTicks) / static_cast<double>(hz));

        QJsonObject row;
        row.insert("pid", static_cast<qint64>(pid));
        row.insert("ppid", static_cast<qint64>(ppid));
        row.insert("name", procName);
        row.insert("executable", exePath);
        row.insert("command_line", cmdline);
        row.insert("cpu_percent", cpuPercent);
        row.insert("memory_percent", memPercent);
        row.insert("threads", threadCount);
        row.insert("uptime_seconds", procUptime);
        row.insert("uptime_human", uptimeString(procUptime));
        row.insert("ros_domain_id", domainId);
        row.insert("is_ros", ros);
        row.insert("node_name", nodeName);
        row.insert("namespace", nodeNs);
        row.insert("package", packageName);
        row.insert("workspace_origin", workspace);
        row.insert("launch_source", launchSource);
        rows.push_back(row);
    }

    std::sort(rows.begin(), rows.end(), [](const QJsonObject& a, const QJsonObject& b) {
        return a.value("cpu_percent").toDouble() > b.value("cpu_percent").toDouble();
    });

    previousProcJiffies_ = currentProcJiffies;
    previousTotalJiffies_ = currentTotalJiffies;
    firstCpuSample_ = false;

    QJsonArray result;
    for (const QJsonObject& row : rows) {
        result.append(row);
    }
    return result;
#endif
}

QJsonArray ProcessManager::filterRosProcesses(const QJsonArray& processes) const {
    QJsonArray rosOnly;
    for (const QJsonValue& value : processes) {
        const QJsonObject obj = value.toObject();
        if (obj.value("is_ros").toBool()) {
            rosOnly.append(obj);
        }
    }
    return rosOnly;
}

QJsonArray ProcessManager::workspaceOrigins(const QJsonArray& processes) const {
    QSet<QString> unique;
    for (const QJsonValue& value : processes) {
        const QString origin = value.toObject().value("workspace_origin").toString();
        if (!origin.isEmpty()) {
            unique.insert(origin);
        }
    }
    QStringList values = unique.values();
    std::sort(values.begin(), values.end());
    QJsonArray result;
    for (const QString& value : values) {
        result.append(value);
    }
    return result;
}

bool ProcessManager::terminateProcess(qint64 pid) const {
#ifdef __linux__
    return ::kill(static_cast<pid_t>(pid), SIGTERM) == 0;
#else
    Q_UNUSED(pid);
    return false;
#endif
}

bool ProcessManager::forceKillProcess(qint64 pid) const {
#ifdef __linux__
    return ::kill(static_cast<pid_t>(pid), SIGKILL) == 0;
#else
    Q_UNUSED(pid);
    return false;
#endif
}

bool ProcessManager::killProcessTree(qint64 pid, bool force) const {
#ifndef __linux__
    Q_UNUSED(pid);
    Q_UNUSED(force);
    return false;
#else
    QSet<qint64> children;
    collectChildrenRecursive(pid, children);

    bool success = true;
    for (qint64 child : children) {
        const int signalCode = force ? SIGKILL : SIGTERM;
        if (::kill(static_cast<pid_t>(child), signalCode) != 0) {
            success = false;
        }
    }
    const int rootSignal = force ? SIGKILL : SIGTERM;
    if (::kill(static_cast<pid_t>(pid), rootSignal) != 0) {
        success = false;
    }
    return success;
#endif
}

}  // namespace rrcc
