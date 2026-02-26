#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace rrcc {

class SessionRecorder {
public:
    SessionRecorder() = default;

    QJsonObject start(const QString& sessionName);
    QJsonObject stop();
    void recordSample(const QJsonObject& snapshot);
    QJsonObject status() const;
    QJsonObject exportSession(const QString& format = "json") const;

private:
    bool active_ = false;
    QString sessionName_;
    QString startedUtc_;
    QString endedUtc_;
    QJsonArray samples_;
    int maxSamples_ = 5000;
};

}  // namespace rrcc

