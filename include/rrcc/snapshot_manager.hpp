#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace rrcc {

class SnapshotManager {
public:
    SnapshotManager() = default;

    QJsonObject buildSnapshot(
        const QJsonArray& processes,
        const QJsonArray& domains,
        const QJsonObject& graph,
        const QJsonObject& tfNav2,
        const QJsonObject& system,
        const QJsonObject& health,
        const QJsonObject& parameters) const;

    QJsonObject exportSnapshot(const QJsonObject& snapshot, const QString& format) const;

private:
    static QString toYaml(const QJsonValue& value, int indent = 0);
};

}  // namespace rrcc
