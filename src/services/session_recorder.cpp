#include "rrcc/session_recorder.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>

namespace rrcc {

QJsonObject SessionRecorder::start(const QString& sessionName) {
    active_ = true;
    sessionName_ = sessionName.trimmed().isEmpty() ? "RosScope_session" : sessionName.trimmed();
    startedUtc_ = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    endedUtc_.clear();
    samples_ = QJsonArray{};
    return status();
}

QJsonObject SessionRecorder::stop() {
    active_ = false;
    endedUtc_ = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    return status();
}

void SessionRecorder::recordSample(const QJsonObject& snapshot) {
    if (!active_) {
        return;
    }
    QJsonObject compact = snapshot;
    compact.remove("logs");
    samples_.append(compact);
    while (samples_.size() > maxSamples_) {
        samples_.removeAt(0);
    }
}

QJsonObject SessionRecorder::status() const {
    return {
        {"active", active_},
        {"session_name", sessionName_},
        {"started_utc", startedUtc_},
        {"ended_utc", endedUtc_},
        {"sample_count", samples_.size()},
    };
}

QJsonObject SessionRecorder::exportSession(const QString& format) const {
    if (samples_.isEmpty()) {
        return {
            {"success", false},
            {"error", "No recorded samples to export."},
        };
    }

    const QString ext = format.trimmed().toLower() == "yaml" ? "yaml" : "json";
    QDir dir(QDir::currentPath());
    if (!dir.exists("sessions")) {
        dir.mkpath("sessions");
    }

    const QString ts = QDateTime::currentDateTimeUtc().toString("yyyyMMdd_HHmmss");
    const QString path =
        dir.filePath(QString("sessions/%1_%2.%3").arg(sessionName_, ts, ext));

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return {
            {"success", false},
            {"error", "Failed to open session file."},
            {"path", path},
        };
    }

    QJsonObject payload;
    payload.insert("session_name", sessionName_);
    payload.insert("started_utc", startedUtc_);
    payload.insert("ended_utc", endedUtc_);
    payload.insert("samples", samples_);

    if (ext == "json") {
        file.write(QJsonDocument(payload).toJson(QJsonDocument::Indented));
    } else {
        // MVP: yaml export writes JSON string payload for compatibility.
        file.write(QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Indented)).toUtf8());
    }
    file.close();

    return {
        {"success", true},
        {"path", path},
        {"sample_count", samples_.size()},
    };
}

}  // namespace rrcc
