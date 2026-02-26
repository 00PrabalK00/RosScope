#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QString>

namespace rrcc {

class RemoteMonitor {
public:
    struct Target {
        QString name;
        QString host;
        QString user;
        int port = 22;
        QString domainId = "0";
        QString rosSetup = "/opt/ros/humble/setup.bash";
    };

    RemoteMonitor() = default;

    QJsonObject loadTargetsFromFile(const QString& filePath);
    void setTargets(const QJsonArray& targets);
    [[nodiscard]] QJsonArray targets() const;

    QJsonObject collectFleetStatus(int timeoutMs = 4500) const;
    QJsonObject executeRemoteAction(
        const QString& targetName,
        const QString& action,
        const QString& domainId = "0",
        int timeoutMs = 4500);
    QJsonObject resumeQueuedActions(int budget = 3, int timeoutMs = 4500);

private:
    struct CircuitState {
        int failures = 0;
        qint64 openUntilMs = 0;
    };

    static QJsonObject toJson(const Target& target);
    static Target fromJson(const QJsonObject& object);
    static QString hostKey(const Target& target);
    QJsonObject executeRemoteActionInternal(
        const QString& targetName,
        const QString& action,
        const QString& domainId,
        int timeoutMs,
        bool allowQueueWrite);
    bool isCircuitOpen(const QString& key) const;
    void onCircuitSuccess(const QString& key) const;
    void onCircuitFailure(const QString& key) const;
    QString queuePath() const;
    void loadQueue();
    void persistQueue() const;
    void enqueueOfflineAction(const QJsonObject& action);

    QJsonArray targets_;
    mutable QMap<QString, CircuitState> circuit_;
    mutable QJsonArray offlineQueue_;
    int maxOfflineQueue_ = 600;
    int maxRetries_ = 3;
    int circuitFailureThreshold_ = 4;
    int circuitCooldownMs_ = 30000;
};

}  // namespace rrcc
