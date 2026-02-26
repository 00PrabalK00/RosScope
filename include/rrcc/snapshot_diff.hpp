#pragma once

#include <QJsonObject>
#include <QString>

namespace rrcc {

class SnapshotDiff {
public:
    SnapshotDiff() = default;

    QJsonObject compare(const QJsonObject& left, const QJsonObject& right) const;
    QJsonObject compareFiles(const QString& leftPath, const QString& rightPath) const;

private:
    static QJsonArray nodeList(const QJsonObject& snapshot);
    static QJsonArray topicList(const QJsonObject& snapshot);
    static QJsonArray domainList(const QJsonObject& snapshot);
    static QJsonObject paramHashes(const QJsonObject& snapshot);
};

}  // namespace rrcc

