#include "rrcc/process_manager.hpp"

#include <QDir>
#include <QElapsedTimer>
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

#include "rrcc/telemetry.hpp"

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

    // Guard expensive /proc/<pid>/maps scanning; this path can be very large
    // and cause memory pressure on machines with many heavy processes.
    if (!haystack.contains("ros")
        && !haystack.contains("rcl")
        && !haystack.contains("dds")
        && !haystack.contains("fast")
        && !haystack.contains("cyclone")) {
        return false;
    }

    QFile mapsFile(pidPath + "/maps");
    if (!mapsFile.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray mapsChunk = mapsFile.read(256 * 1024);  // bounded read
    mapsFile.close();
    const QString maps = QString::fromUtf8(mapsChunk).toLower();
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

QVector<qint64> ProcessManager::listProcPids() {
    QVector<qint64> pids;
    const QDir procDir("/proc");
    const QStringList entries = procDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    pids.reserve(entries.size());
    for (const QString& entry : entries) {
        if (isNumeric(entry)) {
            pids.push_back(entry.toLongLong());
        }
    }
    return pids;
}

int ProcessManager::countOpenFds(const QString& pidPath) {
    QDir fdDir(pidPath + "/fd");
    return fdDir.entryList(QDir::NoDotAndDotDot | QDir::AllEntries).size();
}

QString ProcessManager::readCgroup(const QString& pidPath) {
    return readFile(pidPath + "/cgroup").left(2048);
}

ProcessManager::ProcHeavy ProcessManager::fetchHeavyDetails(qint64 pid) const {
    const QString pidPath = "/proc/" + QString::number(pid);
    ProcHeavy heavy;
    heavy.cmdline = readCmdline(pidPath);
    heavy.env = readEnviron(pidPath);
    heavy.cgroup = readCgroup(pidPath);
    heavy.openFdCount = countOpenFds(pidPath);
    heavy.threadCount = readStatus(pidPath).value("Threads").toString().toInt();
    return heavy;
}

void ProcessManager::touchHeavyCache(qint64 pid, const ProcHeavy& heavy) {
    heavyCache_.insert(pid, heavy);
    heavyLru_.enqueue(pid);
    evictHeavyCacheIfNeeded();
}

void ProcessManager::evictHeavyCacheIfNeeded() {
    while (heavyCache_.size() > maxHeavyCacheEntries_) {
        if (heavyLru_.isEmpty()) {
            break;
        }
        const qint64 victim = heavyLru_.dequeue();
        heavyCache_.remove(victim);
    }
}

QVector<ProcessManager::HeapEntry> ProcessManager::topKCpu(int k) const {
    auto cmp = [](const HeapEntry& a, const HeapEntry& b) { return a.metric > b.metric; };
    std::vector<HeapEntry> heap;
    heap.reserve(static_cast<size_t>(k) + 1);
    for (auto it = pidIndex_.constBegin(); it != pidIndex_.constEnd(); ++it) {
        const HeapEntry entry{it.value().cpuPercent, it.key()};
        if (static_cast<int>(heap.size()) < k) {
            heap.push_back(entry);
            std::push_heap(heap.begin(), heap.end(), cmp);
        } else if (entry.metric > heap.front().metric) {
            std::pop_heap(heap.begin(), heap.end(), cmp);
            heap.back() = entry;
            std::push_heap(heap.begin(), heap.end(), cmp);
        }
    }
    QVector<HeapEntry> out;
    out.reserve(static_cast<qsizetype>(heap.size()));
    for (const HeapEntry& e : heap) {
        out.push_back(e);
    }
    return out;
}

QVector<ProcessManager::HeapEntry> ProcessManager::topKMemory(int k) const {
    auto cmp = [](const HeapEntry& a, const HeapEntry& b) { return a.metric > b.metric; };
    std::vector<HeapEntry> heap;
    heap.reserve(static_cast<size_t>(k) + 1);
    for (auto it = pidIndex_.constBegin(); it != pidIndex_.constEnd(); ++it) {
        const HeapEntry entry{static_cast<double>(it.value().rssKb), it.key()};
        if (static_cast<int>(heap.size()) < k) {
            heap.push_back(entry);
            std::push_heap(heap.begin(), heap.end(), cmp);
        } else if (entry.metric > heap.front().metric) {
            std::pop_heap(heap.begin(), heap.end(), cmp);
            heap.back() = entry;
            std::push_heap(heap.begin(), heap.end(), cmp);
        }
    }
    QVector<HeapEntry> out;
    out.reserve(static_cast<qsizetype>(heap.size()));
    for (const HeapEntry& e : heap) {
        out.push_back(e);
    }
    return out;
}

void ProcessManager::prefetchHeavyForTopK(
    const QVector<HeapEntry>& topCpu,
    const QVector<HeapEntry>& topMem,
    int budget) {
    QSet<qint64> candidate;
    for (const HeapEntry& e : topCpu) {
        candidate.insert(e.pid);
    }
    for (const HeapEntry& e : topMem) {
        candidate.insert(e.pid);
    }
    int used = 0;
    for (qint64 pid : candidate) {
        if (used >= budget) {
            break;
        }
        if (heavyCache_.contains(pid) || !pidIndex_.contains(pid)) {
            continue;
        }
        touchHeavyCache(pid, fetchHeavyDetails(pid));
        used++;
    }
}

bool ProcessManager::collectLiteForPid(qint64 pid, bool deepRosInspection) {
    const QString pidPath = "/proc/" + QString::number(pid);
    const QString statLine = readFile(pidPath + "/stat");
    if (statLine.isEmpty()) {
        return false;
    }
    const int leftParen = statLine.indexOf('(');
    const int rightParen = statLine.lastIndexOf(')');
    if (leftParen < 0 || rightParen < 0 || rightParen <= leftParen) {
        return false;
    }

    ProcLite rec = pidIndex_.value(pid);
    rec.pid = pid;
    rec.name = statLine.mid(leftParen + 1, rightParen - leftParen - 1).left(64);
    const QStringList fields = statLine.mid(rightParen + 2).trimmed().split(' ', Qt::SkipEmptyParts);
    if (fields.size() < 20) {
        return false;
    }
    rec.state = fields[0];
    rec.ppid = fields[1].toLongLong();
    rec.threads = fields[17].toInt();
    const qulonglong utime = fields[11].toULongLong();
    const qulonglong stime = fields[12].toULongLong();
    const qulonglong starttimeTicks = fields[19].toULongLong();
    const qulonglong procJiffies = utime + stime;

    const qulonglong deltaTotal = tickTotalJiffies_ - previousTotalJiffies_;
    if (!firstCpuSample_ && deltaTotal > 0 && previousProcJiffies_.contains(pid)) {
        const qulonglong deltaProc = procJiffies - previousProcJiffies_.value(pid);
        rec.cpuPercent =
            (100.0 * static_cast<double>(deltaProc) * static_cast<double>(cpuCores_))
            / static_cast<double>(deltaTotal);
        if (rec.cpuPercent < 0.0) {
            rec.cpuPercent = 0.0;
        }
    } else {
        rec.cpuPercent = 0.0;
    }
    previousProcJiffies_.insert(pid, procJiffies);

    const QJsonObject status = readStatus(pidPath);
    rec.rssKb = status.value("VmRSS").toString().split(' ', Qt::SkipEmptyParts).value(0).toULongLong();
    rec.uptimeSeconds = tickUptimeSeconds_
        - (static_cast<double>(starttimeTicks) / static_cast<double>(clockTicks_));

    if (deepRosInspection) {
        rec.commandLine = readCmdline(pidPath).left(320);
        rec.executable = readExePath(pidPath);
        const QMap<QString, QString> env = readEnviron(pidPath);
        rec.domainId = env.value("ROS_DOMAIN_ID", "0");
        rec.isRos = isRosProcess(pidPath, rec.executable, rec.commandLine, env);
        rec.nodeName = detectNodeName(rec.commandLine);
        rec.nameSpace = detectNamespace(rec.commandLine);
        rec.workspaceOrigin = detectWorkspaceOrigin(rec.executable, env);
        rec.packageName = detectPackage(rec.executable, rec.commandLine);
        rec.launchSource = detectLaunchSource(rec.commandLine);
    } else {
        rec.commandLine.clear();
        rec.executable.clear();
        rec.domainId = "0";
        rec.isRos = false;
        rec.nodeName.clear();
        rec.nameSpace = "/";
        rec.workspaceOrigin.clear();
        rec.packageName.clear();
        rec.launchSource.clear();
    }

    rec.lastSeenTick = tickCounter_;
    pidIndex_.insert(pid, rec);
    return true;
}

void ProcessManager::refreshIncremental(bool deepRosInspection) {
    tickCounter_++;
    if (clockTicks_ <= 0) {
        clockTicks_ = sysconf(_SC_CLK_TCK);
    }
    if (cpuCores_ <= 0) {
        cpuCores_ = std::max(1, static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN)));
    }

    const qulonglong currentTotalJiffies = totalJiffies();
    memTotalKb_ = memoryTotalKb();
    tickTotalJiffies_ = currentTotalJiffies;
    tickUptimeSeconds_ = systemUptimeSeconds();

    const QVector<qint64> currentPids = listProcPids();
    for (qint64 pid : currentPids) {
        const bool isNewPid = !pidIndex_.contains(pid);
        ProcLite rec = pidIndex_.value(pid);
        rec.pid = pid;
        rec.lastSeenTick = tickCounter_;
        pidIndex_.insert(pid, rec);
        if (isNewPid) {
            rrPids_.push_back(pid);
        }
    }

    int updated = 0;
    const int rrCount = rrPids_.size();
    while (updated < updateBudgetPerTick_ && rrCount > 0) {
        if (rrCursor_ >= rrPids_.size()) {
            rrCursor_ = 0;
        }
        const qint64 pid = rrPids_.at(rrCursor_++);
        if (!pidIndex_.contains(pid)) {
            continue;
        }
        if (collectLiteForPid(pid, deepRosInspection)) {
            updated++;
        }
    }

    QVector<qint64> deadPids;
    deadPids.reserve(pidIndex_.size() / 8);
    for (auto it = pidIndex_.constBegin(); it != pidIndex_.constEnd(); ++it) {
        if (it.value().lastSeenTick != tickCounter_) {
            deadPids.push_back(it.key());
        }
    }
    for (qint64 pid : deadPids) {
        pidIndex_.remove(pid);
        previousProcJiffies_.remove(pid);
        heavyCache_.remove(pid);
    }

    rrPids_.erase(
        std::remove_if(rrPids_.begin(), rrPids_.end(), [this](qint64 pid) { return !pidIndex_.contains(pid); }),
        rrPids_.end());
    if (rrCursor_ >= rrPids_.size()) {
        rrCursor_ = 0;
    }

    const QVector<HeapEntry> topCpu = topKCpu(20);
    const QVector<HeapEntry> topMem = topKMemory(20);
    prefetchHeavyForTopK(topCpu, topMem, 4);

    const qint64 deltaTotal = static_cast<qint64>(currentTotalJiffies - previousTotalJiffies_);
    previousTotalJiffies_ = currentTotalJiffies;
    firstCpuSample_ = false;

    if (deltaTotal <= 0 || updated < (updateBudgetPerTick_ / 2)) {
        updateBudgetPerTick_ = qMax(minBudget_, static_cast<int>(updateBudgetPerTick_ * 0.85));
    } else {
        updateBudgetPerTick_ = qMin(maxBudget_, updateBudgetPerTick_ + 12);
    }
}

QJsonObject ProcessManager::toJsonRow(const ProcLite& rec, qulonglong memTotalKb) const {
    QJsonObject row;
    row.insert("pid", rec.pid);
    row.insert("ppid", rec.ppid);
    row.insert("name", rec.name);
    row.insert("state", rec.state);
    row.insert("executable", rec.executable);
    row.insert("command_line", rec.commandLine);
    row.insert("cpu_percent", rec.cpuPercent);
    row.insert("memory_percent", memoryPercentKb(rec.rssKb, memTotalKb));
    row.insert("threads", rec.threads);
    row.insert("uptime_seconds", rec.uptimeSeconds);
    row.insert("uptime_human", uptimeString(rec.uptimeSeconds));
    row.insert("ros_domain_id", rec.domainId);
    row.insert("is_ros", rec.isRos);
    row.insert("node_name", rec.nodeName);
    row.insert("namespace", rec.nameSpace);
    row.insert("package", rec.packageName);
    row.insert("workspace_origin", rec.workspaceOrigin);
    row.insert("launch_source", rec.launchSource);
    return row;
}

QJsonArray ProcessManager::listProcesses(bool rosOnly, const QString& query, bool deepRosInspection) {
#ifndef __linux__
    Q_UNUSED(rosOnly);
    Q_UNUSED(query);
    Q_UNUSED(deepRosInspection);
    return {};
#else
    QElapsedTimer timer;
    timer.start();

    refreshIncremental(deepRosInspection);
    const QString queryLower = query.trimmed().toLower();

    QList<ProcLite> rows;
    rows.reserve(pidIndex_.size());
    for (auto it = pidIndex_.constBegin(); it != pidIndex_.constEnd(); ++it) {
        const ProcLite& rec = it.value();
        if (rosOnly && !rec.isRos) {
            continue;
        }
        if (!queryLower.isEmpty()) {
            const QString searchable =
                QString::number(rec.pid) + " " + rec.name + " " + rec.executable + " " + rec.commandLine;
            if (!searchable.toLower().contains(queryLower)) {
                continue;
            }
        }
        rows.push_back(rec);
    }

    std::sort(rows.begin(), rows.end(), [](const ProcLite& a, const ProcLite& b) {
        return a.cpuPercent > b.cpuPercent;
    });

    QJsonArray result;
    for (const ProcLite& rec : rows) {
        result.append(toJsonRow(rec, memTotalKb_));
    }

    Telemetry::instance().incrementCounter("process.list_queries");
    Telemetry::instance().setGauge("process.last_result_size", static_cast<double>(result.size()));
    Telemetry::instance().setGauge("process.budget_per_tick", static_cast<double>(updateBudgetPerTick_));
    Telemetry::instance().setGauge("process.cache.heavy_size", static_cast<double>(heavyCache_.size()));
    Telemetry::instance().recordDurationMs("process.query_ms", timer.elapsed());
    return result;
#endif
}

QJsonObject ProcessManager::listProcessesPaged(
    bool rosOnly,
    const QString& query,
    bool deepRosInspection,
    int offset,
    int limit,
    bool sortByCpu) {
#ifndef __linux__
    Q_UNUSED(rosOnly);
    Q_UNUSED(query);
    Q_UNUSED(deepRosInspection);
    Q_UNUSED(offset);
    Q_UNUSED(limit);
    Q_UNUSED(sortByCpu);
    return QJsonObject{{"rows", QJsonArray{}}, {"total", 0}};
#else
    QElapsedTimer timer;
    timer.start();

    refreshIncremental(deepRosInspection);
    const QString queryLower = query.trimmed().toLower();
    const int safeOffset = qMax(0, offset);
    const int safeLimit = qMax(1, limit);
    QJsonArray rows;
    int total = 0;

    if (!sortByCpu) {
        // Stream pagination path: avoid copying/sorting the entire process set.
        int emitted = 0;
        const int pageEndExclusive = safeOffset + safeLimit;
        for (auto it = pidIndex_.constBegin(); it != pidIndex_.constEnd(); ++it) {
            const ProcLite& rec = it.value();
            if (rosOnly && !rec.isRos) {
                continue;
            }
            if (!queryLower.isEmpty()) {
                const QString searchable =
                    QString::number(rec.pid) + " " + rec.name + " " + rec.executable + " " + rec.commandLine;
                if (!searchable.toLower().contains(queryLower)) {
                    continue;
                }
            }
            if (total >= safeOffset && total < pageEndExclusive && emitted < safeLimit) {
                rows.append(toJsonRow(rec, memTotalKb_));
                emitted++;
            }
            total++;
        }
    } else {
        QList<ProcLite> filtered;
        filtered.reserve(pidIndex_.size());
        for (auto it = pidIndex_.constBegin(); it != pidIndex_.constEnd(); ++it) {
            const ProcLite& rec = it.value();
            if (rosOnly && !rec.isRos) {
                continue;
            }
            if (!queryLower.isEmpty()) {
                const QString searchable =
                    QString::number(rec.pid) + " " + rec.name + " " + rec.executable + " " + rec.commandLine;
                if (!searchable.toLower().contains(queryLower)) {
                    continue;
                }
            }
            filtered.push_back(rec);
        }
        total = filtered.size();
        std::sort(filtered.begin(), filtered.end(), [](const ProcLite& a, const ProcLite& b) {
            return a.cpuPercent > b.cpuPercent;
        });
        const int end = qMin(total, safeOffset + safeLimit);
        for (int i = safeOffset; i < end; ++i) {
            rows.append(toJsonRow(filtered.at(i), memTotalKb_));
        }
    }

    Telemetry::instance().incrementCounter("process.list_paged_queries");
    Telemetry::instance().setGauge("process.last_result_size", static_cast<double>(rows.size()));
    Telemetry::instance().setGauge("process.last_total_filtered", static_cast<double>(total));
    Telemetry::instance().recordDurationMs("process.query_ms", timer.elapsed());
    return QJsonObject{{"rows", rows}, {"total", total}};
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
