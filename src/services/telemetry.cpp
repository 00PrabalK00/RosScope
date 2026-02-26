#include "rrcc/telemetry.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMutexLocker>

namespace rrcc {

Telemetry& Telemetry::instance() {
    static Telemetry singleton;
    return singleton;
}

void Telemetry::incrementCounter(const QString& key, qint64 delta) {
    QMutexLocker lock(&mutex_);
    const qint64 prev = static_cast<qint64>(counters_.value(key).toDouble(0));
    counters_.insert(key, static_cast<double>(prev + delta));
}

void Telemetry::setGauge(const QString& key, double value) {
    QMutexLocker lock(&mutex_);
    gauges_.insert(key, value);
}

void Telemetry::recordDurationMs(const QString& key, qint64 durationMs) {
    QMutexLocker lock(&mutex_);
    const QJsonObject old = durations_.value(key).toObject();
    const qint64 count = static_cast<qint64>(old.value("count").toDouble(0)) + 1;
    const qint64 total = static_cast<qint64>(old.value("total_ms").toDouble(0)) + durationMs;
    const qint64 max = qMax(static_cast<qint64>(old.value("max_ms").toDouble(0)), durationMs);
    QJsonObject obj;
    obj.insert("count", static_cast<double>(count));
    obj.insert("total_ms", static_cast<double>(total));
    obj.insert("max_ms", static_cast<double>(max));
    obj.insert("avg_ms", count > 0 ? static_cast<double>(total) / static_cast<double>(count) : 0.0);
    durations_.insert(key, obj);
}

void Telemetry::trimEventsLocked() {
    while (events_.size() > maxEvents_) {
        events_.removeFirst();
    }
}

void Telemetry::trimRequestTimesLocked() {
    while (requestTimesMs_.size() > maxRequestSamples_) {
        requestTimesMs_.dequeue();
    }
    const qint64 cutoff = QDateTime::currentMSecsSinceEpoch() - 60000;
    while (!requestTimesMs_.isEmpty() && requestTimesMs_.head() < cutoff) {
        requestTimesMs_.dequeue();
    }
}

void Telemetry::recordEvent(const QString& type, const QJsonObject& payload) {
    QMutexLocker lock(&mutex_);
    QJsonObject row = payload;
    row.insert("type", type);
    row.insert("timestamp_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    row.insert("epoch_ms", static_cast<double>(QDateTime::currentMSecsSinceEpoch()));
    events_.append(row);
    trimEventsLocked();
}

void Telemetry::recordRequest() {
    QMutexLocker lock(&mutex_);
    requestTimesMs_.enqueue(QDateTime::currentMSecsSinceEpoch());
    trimRequestTimesLocked();
}

void Telemetry::setQueueSize(const QString& key, int size) {
    setGauge("queue." + key, static_cast<double>(size));
}

QJsonObject Telemetry::snapshot() const {
    QMutexLocker lock(&mutex_);
    QJsonObject out;
    out.insert("counters", counters_);
    out.insert("gauges", gauges_);
    out.insert("durations", durations_);
    out.insert("events", events_);
    out.insert("requests_per_minute", requestTimesMs_.size());
    out.insert("timestamp_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    return out;
}

QJsonObject Telemetry::exportToFile(const QString& filePath) const {
    const QJsonObject payload = snapshot();

    QFile file(filePath);
    QDir dir = QFileInfo(file).absoluteDir();
    if (!dir.exists()) {
        dir.mkpath(".");
    }

    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return {
            {"success", false},
            {"error", "Failed to open telemetry export path."},
            {"path", filePath},
        };
    }
    file.write(QJsonDocument(payload).toJson(QJsonDocument::Indented));
    file.close();
    return {
        {"success", true},
        {"path", filePath},
    };
}

}  // namespace rrcc
