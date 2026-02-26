#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

namespace rrcc {

class ProcessManager {
public:
    ProcessManager() = default;

    QJsonArray listProcesses(bool rosOnly, const QString& query, bool deepRosInspection = true);
    QJsonObject listProcessesPaged(
        bool rosOnly,
        const QString& query,
        bool deepRosInspection,
        int offset,
        int limit,
        bool sortByCpu = true);
    QJsonArray filterRosProcesses(const QJsonArray& processes) const;
    QJsonArray workspaceOrigins(const QJsonArray& processes) const;

    bool terminateProcess(qint64 pid) const;
    bool forceKillProcess(qint64 pid) const;
    bool killProcessTree(qint64 pid, bool force = true) const;

private:
    struct ProcLite {
        qint64 pid = -1;
        qint64 ppid = -1;
        QString name;
        QString state;
        double cpuPercent = 0.0;
        qulonglong rssKb = 0;
        int threads = 0;
        double uptimeSeconds = 0.0;
        QString domainId = "0";
        bool isRos = false;
        QString nodeName;
        QString nameSpace = "/";
        QString executable;
        QString workspaceOrigin;
        QString packageName;
        QString launchSource;
        QString commandLine;
        qint64 lastSeenTick = 0;
    };

    struct ProcHeavy {
        QString cmdline;
        QMap<QString, QString> env;
        QString cgroup;
        int openFdCount = 0;
        int threadCount = 0;
    };

    struct HeapEntry {
        double metric = 0.0;
        qint64 pid = -1;
    };

    static bool isNumeric(const QString& value);
    static QString readFile(const QString& path);
    static QJsonObject readStatus(const QString& pidPath);
    static QMap<QString, QString> readEnviron(const QString& pidPath);
    static QString readCmdline(const QString& pidPath);
    static QString readExePath(const QString& pidPath);
    static QString detectWorkspaceOrigin(
        const QString& exePath,
        const QMap<QString, QString>& env);
    static QString detectPackage(const QString& exePath, const QString& cmdline);
    static QString detectLaunchSource(const QString& cmdline);
    static QString detectNodeName(const QString& cmdline);
    static QString detectNamespace(const QString& cmdline);
    static bool isRosProcess(
        const QString& pidPath,
        const QString& exePath,
        const QString& cmdline,
        const QMap<QString, QString>& env);
    static double memoryPercentKb(qulonglong vmRssKb, qulonglong memTotalKb);
    static QString uptimeString(double seconds);
    static qulonglong totalJiffies();
    static qulonglong memoryTotalKb();
    static double systemUptimeSeconds();
    static QList<qint64> listChildren(qint64 parentPid);
    static void collectChildrenRecursive(qint64 pid, QSet<qint64>& outSet);
    static QVector<qint64> listProcPids();
    static int countOpenFds(const QString& pidPath);
    static QString readCgroup(const QString& pidPath);

    void refreshIncremental(bool deepRosInspection);
    bool collectLiteForPid(qint64 pid, bool deepRosInspection);
    QJsonObject toJsonRow(const ProcLite& rec, qulonglong memTotalKb) const;
    QVector<HeapEntry> topKCpu(int k) const;
    QVector<HeapEntry> topKMemory(int k) const;
    void prefetchHeavyForTopK(const QVector<HeapEntry>& topCpu, const QVector<HeapEntry>& topMem, int budget);
    ProcHeavy fetchHeavyDetails(qint64 pid) const;
    void touchHeavyCache(qint64 pid, const ProcHeavy& heavy);
    void evictHeavyCacheIfNeeded();

    QHash<qint64, qulonglong> previousProcJiffies_;
    qulonglong previousTotalJiffies_ = 0;
    bool firstCpuSample_ = true;
    qulonglong memTotalKb_ = 0;
    long clockTicks_ = 100;
    int cpuCores_ = 1;

    QHash<qint64, ProcLite> pidIndex_;
    QVector<qint64> rrPids_;
    int rrCursor_ = 0;
    qint64 tickCounter_ = 0;
    int updateBudgetPerTick_ = 260;
    int minBudget_ = 60;
    int maxBudget_ = 900;

    QHash<qint64, ProcHeavy> heavyCache_;
    QQueue<qint64> heavyLru_;
    int maxHeavyCacheEntries_ = 256;
    qulonglong tickTotalJiffies_ = 0;
    double tickUptimeSeconds_ = 0.0;
};

}  // namespace rrcc
