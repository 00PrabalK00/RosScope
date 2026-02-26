#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QMutex>
#include <QQueue>
#include <QString>

namespace rrcc {

class Telemetry final {
public:
    static Telemetry& instance();

    void incrementCounter(const QString& key, qint64 delta = 1);
    void setGauge(const QString& key, double value);
    void recordDurationMs(const QString& key, qint64 durationMs);
    void recordEvent(const QString& type, const QJsonObject& payload = {});
    void recordRequest();
    void setQueueSize(const QString& key, int size);

    [[nodiscard]] QJsonObject snapshot() const;
    QJsonObject exportToFile(const QString& filePath) const;

private:
    Telemetry() = default;

    struct DurationStats {
        qint64 count = 0;
        qint64 totalMs = 0;
        qint64 maxMs = 0;
    };

    void trimEventsLocked();
    void trimRequestTimesLocked();

    mutable QMutex mutex_;
    QJsonObject counters_;
    QJsonObject gauges_;
    QJsonObject durations_;
    QJsonArray events_;
    QQueue<qint64> requestTimesMs_;

    int maxEvents_ = 1500;
    int maxRequestSamples_ = 2400;
};

}  // namespace rrcc
