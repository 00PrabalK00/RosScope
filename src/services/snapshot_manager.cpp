#include "rrcc/snapshot_manager.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>

namespace rrcc {

QJsonObject SnapshotManager::buildSnapshot(
    const QJsonArray& processes,
    const QJsonArray& domains,
    const QJsonObject& graph,
    const QJsonObject& tfNav2,
    const QJsonObject& system,
    const QJsonObject& health,
    const QJsonObject& parameters) const {
    QJsonObject snapshot;
    snapshot.insert("timestamp_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    snapshot.insert("processes", processes);
    snapshot.insert("domains", domains);
    snapshot.insert("graph", graph);
    snapshot.insert("tf_nav2", tfNav2);
    snapshot.insert("parameters", parameters);
    snapshot.insert("system", system);
    snapshot.insert("health", health);
    return snapshot;
}

QString SnapshotManager::toYaml(const QJsonValue& value, int indent) {
    const QString pad(indent, ' ');
    if (value.isObject()) {
        QString out;
        const QJsonObject obj = value.toObject();
        for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
            if (it.value().isObject() || it.value().isArray()) {
                out += QString("%1%2:\n%3").arg(pad, it.key(), toYaml(it.value(), indent + 2));
            } else {
                out += QString("%1%2: %3\n").arg(pad, it.key(), toYaml(it.value(), 0).trimmed());
            }
        }
        return out;
    }

    if (value.isArray()) {
        QString out;
        const QJsonArray arr = value.toArray();
        for (const QJsonValue& item : arr) {
            if (item.isObject() || item.isArray()) {
                out += QString("%1-\n%2").arg(pad, toYaml(item, indent + 2));
            } else {
                out += QString("%1- %2\n").arg(pad, toYaml(item, 0).trimmed());
            }
        }
        return out;
    }

    if (value.isString()) {
        QString escaped = value.toString();
        return QString("\"%1\"").arg(escaped.replace('"', "\\\""));
    }
    if (value.isDouble()) {
        return QString::number(value.toDouble(), 'f', 4);
    }
    if (value.isBool()) {
        return value.toBool() ? "true" : "false";
    }
    if (value.isNull() || value.isUndefined()) {
        return "null";
    }
    return {};
}

QJsonObject SnapshotManager::exportSnapshot(
    const QJsonObject& snapshot,
    const QString& format) const {
    const QString ts = QDateTime::currentDateTimeUtc().toString("yyyyMMdd_HHmmss");
    const QString ext = format.trimmed().toLower() == "yaml" ? "yaml" : "json";

    QDir dir(QDir::currentPath());
    if (!dir.exists("snapshots")) {
        dir.mkpath("snapshots");
    }

    const QString path = dir.filePath(QString("snapshots/roscoppe_snapshot_%1.%2").arg(ts, ext));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return {
            {"success", false},
            {"path", path},
            {"error", "Failed to open snapshot file for writing."},
        };
    }

    if (ext == "json") {
        file.write(QJsonDocument(snapshot).toJson(QJsonDocument::Indented));
    } else {
        file.write(toYaml(snapshot, 0).toUtf8());
    }
    file.close();

    return {
        {"success", true},
        {"path", path},
        {"format", ext},
    };
}

}  // namespace rrcc
