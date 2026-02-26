#pragma once

#include <QJsonArray>
#include <QJsonObject>
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
        int timeoutMs = 4500) const;

private:
    static QJsonObject toJson(const Target& target);
    static Target fromJson(const QJsonObject& object);
    static QString hostKey(const Target& target);

    QJsonArray targets_;
};

}  // namespace rrcc

