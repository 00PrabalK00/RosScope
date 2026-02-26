#pragma once

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace rrcc {

class DiagnosticsEngine {
public:
    DiagnosticsEngine() = default;

    QJsonObject evaluate(
        const QString& domainId,
        const QJsonArray& processes,
        const QJsonArray& domains,
        const QJsonObject& graph,
        const QJsonObject& tfNav2,
        const QJsonObject& system,
        const QJsonObject& health,
        const QJsonObject& parameters,
        bool deepSampling,
        int pollIntervalMs);

    void setExpectedProfile(const QJsonObject& expectedProfile);
    [[nodiscard]] QJsonObject expectedProfile() const;

private:
    struct TransitionState {
        QString state;
        qint64 sinceMs = 0;
    };

    QJsonObject parameterDrift(const QJsonObject& parameters);
    QJsonObject topicRateAnalyzer(const QString& domainId, const QJsonObject& graph, bool deepSampling);
    QJsonObject qosMismatchDetector(const QJsonObject& graph) const;
    QJsonObject lifecycleTimeline(const QJsonObject& tfNav2);
    QJsonObject executorLoadMonitor(const QJsonArray& processes, const QJsonObject& graph) const;
    QJsonObject crossCorrelationTimeline(
        const QJsonObject& system,
        const QJsonObject& graph,
        const QJsonObject& tfNav2);
    QJsonObject memoryLeakDetection(const QJsonArray& processes);
    QJsonObject ddsParticipantInspector(const QJsonArray& domains, const QJsonObject& health);
    QJsonObject networkSaturationMonitor(const QJsonObject& system, int pollIntervalMs);
    QJsonObject softSafetyBoundary(const QJsonObject& tfNav2, const QJsonObject& topicRates);
    QJsonObject workspaceTools(const QJsonArray& processes) const;
    QJsonObject actionMonitor(const QJsonObject& tfNav2, const QJsonObject& graph) const;
    QJsonObject tfDriftMonitor(const QJsonObject& tfNav2) const;
    QJsonObject runtimeFingerprint(
        const QJsonObject& graph,
        const QJsonObject& tfNav2,
        const QJsonObject& system) const;
    QJsonObject deterministicLaunchValidation(const QJsonObject& graph) const;
    QJsonObject dependencyImpactMap(const QJsonObject& graph) const;
    static int runtimeStabilityScore(
        const QJsonObject& health,
        const QJsonObject& topicRates,
        const QJsonObject& memoryLeaks,
        const QJsonObject& network);

    static QString stableHash(const QString& value);
    static double parseAverageRate(const QString& text);
    static double parseAverageBandwidth(const QString& text);
    static double linearSlope(const QVector<double>& values);

    QJsonObject expectedProfile_;
    QHash<QString, QString> parameterHashesByNode_;
    QHash<QString, QVector<double>> topicRateHistory_;
    QHash<QString, double> lastTopicBandwidthByTopic_;
    QHash<QString, TransitionState> lifecycleStateByNode_;
    QHash<QString, QJsonArray> lifecycleEventsByNode_;
    QHash<QString, QVector<double>> memoryHistoryByNode_;
    QHash<QString, qint64> previousRxBytesByIface_;
    QHash<QString, qint64> previousTxBytesByIface_;
    QHash<QString, int> previousParticipantsByDomain_;
    QJsonArray timeline_;
    int timelineLimit_ = 600;
};

}  // namespace rrcc
