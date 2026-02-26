#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>

namespace rrcc {

class ProcessManager {
public:
    ProcessManager() = default;

    QJsonArray listProcesses(bool rosOnly, const QString& query, bool deepRosInspection = true);
    QJsonArray filterRosProcesses(const QJsonArray& processes) const;
    QJsonArray workspaceOrigins(const QJsonArray& processes) const;

    bool terminateProcess(qint64 pid) const;
    bool forceKillProcess(qint64 pid) const;
    bool killProcessTree(qint64 pid, bool force = true) const;

private:
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

    QHash<qint64, qulonglong> previousProcJiffies_;
    qulonglong previousTotalJiffies_ = 0;
    bool firstCpuSample_ = true;
};

}  // namespace rrcc
