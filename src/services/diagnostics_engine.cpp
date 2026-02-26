#include "rrcc/diagnostics_engine.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <numeric>

#include "rrcc/command_runner.hpp"

namespace rrcc {

namespace {

QString hashText(const QString& value) {
    return QString::fromLatin1(
        QCryptographicHash::hash(value.toUtf8(), QCryptographicHash::Sha256).toHex());
}

double parseAverageRateText(const QString& text) {
    QRegularExpression re("average rate:\\s*([0-9]+(?:\\.[0-9]+)?)");
    const QRegularExpressionMatch m = re.match(text);
    return m.hasMatch() ? m.captured(1).toDouble() : -1.0;
}

double parseBandwidthBps(const QString& text) {
    QRegularExpression re("([0-9]+(?:\\.[0-9]+)?)\\s*(B|KB|MB|GB)/s");
    const QRegularExpressionMatch m = re.match(text);
    if (!m.hasMatch()) {
        return -1.0;
    }
    const double v = m.captured(1).toDouble();
    const QString u = m.captured(2);
    if (u == "GB") {
        return v * 1024.0 * 1024.0 * 1024.0;
    }
    if (u == "MB") {
        return v * 1024.0 * 1024.0;
    }
    if (u == "KB") {
        return v * 1024.0;
    }
    return v;
}

double slope(const QVector<double>& values) {
    if (values.size() < 3) {
        return 0.0;
    }
    const int n = values.size();
    double sx = 0.0;
    double sy = 0.0;
    double sxy = 0.0;
    double sxx = 0.0;
    for (int i = 0; i < n; ++i) {
        sx += i;
        sy += values[i];
        sxy += i * values[i];
        sxx += i * i;
    }
    const double d = (n * sxx) - (sx * sx);
    return std::abs(d) < 1e-9 ? 0.0 : ((n * sxy) - (sx * sy)) / d;
}

double bpsToMbps(double bps) {
    return bps * 8.0 / (1024.0 * 1024.0);
}

}  // namespace

QJsonObject DiagnosticsEngine::evaluate(
    const QString& domainId,
    const QJsonArray& processes,
    const QJsonArray& domains,
    const QJsonObject& graph,
    const QJsonObject& tfNav2,
    const QJsonObject& system,
    const QJsonObject& health,
    const QJsonObject& parameters,
    bool deepSampling,
    int pollIntervalMs) {
    const QJsonObject paramState = parameterDrift(parameters);
    const QJsonObject rateState = topicRateAnalyzer(domainId, graph, deepSampling);
    const QJsonObject qosState = qosMismatchDetector(graph);
    const QJsonObject lifecycleState = lifecycleTimeline(tfNav2);
    const QJsonObject executorState = executorLoadMonitor(processes, graph);
    const QJsonObject correlationState = crossCorrelationTimeline(system, graph, tfNav2);
    const QJsonObject leakState = memoryLeakDetection(processes);
    const QJsonObject ddsState = ddsParticipantInspector(domains, health);
    const QJsonObject netState = networkSaturationMonitor(system, pollIntervalMs);
    const QJsonObject safetyState = softSafetyBoundary(tfNav2, rateState);
    const QJsonObject workspaceState = workspaceTools(processes);
    const QJsonObject actionState = actionMonitor(tfNav2, graph);
    const QJsonObject tfState = tfDriftMonitor(tfNav2);
    const QJsonObject fingerprintState = runtimeFingerprint(graph, tfNav2, system);
    const QJsonObject launchState = deterministicLaunchValidation(graph);
    const QJsonObject impactState = dependencyImpactMap(graph);

    const int stability = runtimeStabilityScore(health, rateState, leakState, netState);

    QJsonObject out;
    out.insert("parameter_drift", paramState);
    out.insert("topic_rate_analyzer", rateState);
    out.insert("qos_mismatch_detector", qosState);
    out.insert("lifecycle_timeline", lifecycleState);
    out.insert("executor_load_monitor", executorState);
    out.insert("cross_correlation_timeline", correlationState);
    out.insert("memory_leak_detection", leakState);
    out.insert("dds_participant_inspector", ddsState);
    out.insert("network_saturation_monitor", netState);
    out.insert("soft_safety_boundary", safetyState);
    out.insert("workspace_tools", workspaceState);
    out.insert("action_monitor", actionState);
    out.insert("tf_drift_monitor", tfState);
    out.insert("runtime_fingerprint", fingerprintState);
    out.insert("deterministic_launch_validation", launchState);
    out.insert("dependency_impact_map", impactState);
    out.insert("runtime_stability_score", stability);
    out.insert("expected_profile", expectedProfile_);
    return out;
}

void DiagnosticsEngine::setExpectedProfile(const QJsonObject& expectedProfile) {
    expectedProfile_ = expectedProfile;
}

QJsonObject DiagnosticsEngine::expectedProfile() const {
    return expectedProfile_;
}

QJsonObject DiagnosticsEngine::parameterDrift(const QJsonObject& parameters) {
    QJsonArray changes;
    QSet<QString> now;
    for (auto it = parameters.constBegin(); it != parameters.constEnd(); ++it) {
        now.insert(it.key());
        const QString h = hashText(it.value().toString());
        if (!parameterHashesByNode_.contains(it.key())) {
            parameterHashesByNode_.insert(it.key(), h);
            continue;
        }
        if (parameterHashesByNode_.value(it.key()) != h) {
            changes.append(QJsonObject{
                {"node", it.key()},
                {"old_hash", parameterHashesByNode_.value(it.key())},
                {"new_hash", h},
                {"silent_reload_suspected", true},
            });
            parameterHashesByNode_.insert(it.key(), h);
        }
    }
    for (const QString& key : parameterHashesByNode_.keys()) {
        if (!now.contains(key)) {
            parameterHashesByNode_.remove(key);
        }
    }
    return QJsonObject{{"changed_nodes", changes}, {"change_count", changes.size()}};
}

QJsonObject DiagnosticsEngine::topicRateAnalyzer(
    const QString& domainId,
    const QJsonObject& graph,
    bool deepSampling) {
    QMap<QString, QString> env = {{"ROS_DOMAIN_ID", domainId}};
    const QJsonObject expected = expectedProfile_.value("topic_expected_hz").toObject();
    const int maxTopics = deepSampling ? 12 : 4;
    int sampled = 0;

    QJsonArray metrics;
    QJsonArray dropped;
    QJsonArray underperforming;
    QJsonArray spikes;

    for (const QJsonValue& topicValue : graph.value("topics").toArray()) {
        if (sampled >= maxTopics) {
            break;
        }
        const QString topic = topicValue.toObject().value("topic").toString();
        if (topic.isEmpty()) {
            continue;
        }
        sampled++;

        const CommandResult hz = CommandRunner::run("ros2", {"topic", "hz", topic, "--window", "20"}, 2500, env);
        const CommandResult bw = CommandRunner::run("ros2", {"topic", "bw", topic, "--window", "20"}, 2500, env);
        const double actual = hz.success() ? parseAverageRateText(hz.stdoutText) : -1.0;
        const double bandwidth = bw.success() ? parseBandwidthBps(bw.stdoutText) : -1.0;
        if (bandwidth > 0.0) {
            lastTopicBandwidthByTopic_.insert(topic, bandwidth);
        }

        QVector<double> history = topicRateHistory_.value(topic);
        if (actual >= 0.0) {
            history.push_back(actual);
            while (history.size() > 100) {
                history.pop_front();
            }
            topicRateHistory_.insert(topic, history);
        }
        const double expectedHz = expected.value(topic).toDouble(-1.0);
        const double histSlope = slope(history);
        const double histMean = history.isEmpty() ? actual : std::accumulate(history.begin(), history.end(), 0.0) / history.size();

        metrics.append(QJsonObject{
            {"topic", topic},
            {"expected_hz", expectedHz},
            {"actual_hz", actual},
            {"trend_slope", histSlope},
            {"mean_hz", histMean},
            {"bandwidth_bps", bandwidth > 0.0 ? bandwidth : lastTopicBandwidthByTopic_.value(topic, -1.0)},
        });

        if (expectedHz > 0.0 && actual >= 0.0 && actual < expectedHz * 0.6) {
            dropped.append(topic);
            underperforming.append(topic);
        }
        if (history.size() >= 5 && std::abs(histSlope) > std::max(0.3, histMean * 0.2)) {
            spikes.append(topic);
        }
    }

    return QJsonObject{
        {"topic_metrics", metrics},
        {"dropped_topics", dropped},
        {"underperforming_publishers", underperforming},
        {"latency_spikes", spikes},
    };
}

QJsonObject DiagnosticsEngine::qosMismatchDetector(const QJsonObject& graph) const {
    QJsonArray mismatches;
    const QJsonObject qos = graph.value("topic_qos").toObject();
    for (auto it = qos.constBegin(); it != qos.constEnd(); ++it) {
        QSet<QString> uniq;
        for (const QJsonValue& value : it.value().toObject().value("qos_profiles").toArray()) {
            const QJsonObject p = value.toObject();
            uniq.insert(p.value("reliability").toString() + "|" + p.value("durability").toString());
        }
        if (uniq.size() > 1) {
            mismatches.append(QJsonObject{{"topic", it.key()}, {"profile_count", uniq.size()}});
        }
    }
    return QJsonObject{{"mismatches", mismatches}, {"mismatch_count", mismatches.size()}};
}

QJsonObject DiagnosticsEngine::lifecycleTimeline(const QJsonObject& tfNav2) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QJsonArray transitions;
    QJsonArray stuck;

    for (const QJsonValue& value : tfNav2.value("nav2").toObject().value("lifecycle_states").toArray()) {
        const QJsonObject row = value.toObject();
        const QString node = row.value("node").toString();
        const QString state = row.value("state").toString();
        if (node.isEmpty()) {
            continue;
        }

        TransitionState prev = lifecycleStateByNode_.value(node);
        if (prev.state != state) {
            QJsonObject event{
                {"node", node},
                {"previous_state", prev.state},
                {"new_state", state},
                {"timestamp_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
            };
            transitions.append(event);
            QJsonArray history = lifecycleEventsByNode_.value(node);
            history.append(event);
            while (history.size() > 120) {
                history.removeAt(0);
            }
            lifecycleEventsByNode_.insert(node, history);
            lifecycleStateByNode_.insert(node, TransitionState{state, now});
        } else if (prev.sinceMs == 0) {
            lifecycleStateByNode_.insert(node, TransitionState{state, now});
        }

        const QString lower = state.toLower();
        const bool transitional = lower.contains("configur") || lower.contains("activat") || lower.contains("deactivat");
        if (transitional && (now - lifecycleStateByNode_.value(node).sinceMs) > 15000) {
            stuck.append(QJsonObject{
                {"node", node},
                {"state", state},
                {"duration_ms", static_cast<qint64>(now - lifecycleStateByNode_.value(node).sinceMs)},
            });
        }
    }

    QJsonObject history;
    for (auto it = lifecycleEventsByNode_.constBegin(); it != lifecycleEventsByNode_.constEnd(); ++it) {
        history.insert(it.key(), it.value());
    }
    return QJsonObject{
        {"transitions", transitions},
        {"stuck_transitional_nodes", stuck},
        {"history_by_node", history},
    };
}

QJsonObject DiagnosticsEngine::executorLoadMonitor(
    const QJsonArray& processes,
    const QJsonObject& graph) const {
    QJsonArray overloaded;
    for (const QJsonValue& value : processes) {
        const QJsonObject proc = value.toObject();
        if (!proc.value("is_ros").toBool()) {
            continue;
        }
        const double cpu = proc.value("cpu_percent").toDouble();
        const int threads = proc.value("threads").toInt();
        if (cpu > 85.0 || threads > 80) {
            overloaded.append(QJsonObject{
                {"pid", static_cast<qint64>(proc.value("pid").toDouble(-1))},
                {"node_name", proc.value("node_name").toString()},
                {"cpu_percent", cpu},
                {"threads", threads},
            });
        }
    }

    const int orphanTopics = graph.value("publishers_without_subscribers").toArray().size();
    return QJsonObject{
        {"overloaded_executors", overloaded},
        {"callback_queue_delay_ms", overloaded.size() * 10 + orphanTopics * 3},
        {"blocking_callbacks", overloaded},
    };
}

QJsonObject DiagnosticsEngine::crossCorrelationTimeline(
    const QJsonObject& system,
    const QJsonObject& graph,
    const QJsonObject& tfNav2) {
    QJsonObject row;
    row.insert("timestamp_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    row.insert("cpu_percent", system.value("cpu").toObject().value("usage_percent").toDouble());
    row.insert("orphan_topics", graph.value("publishers_without_subscribers").toArray().size());
    row.insert("tf_warnings", tfNav2.value("tf_warnings").toArray().size());
    row.insert("goal_active", tfNav2.value("nav2").toObject().value("goal_active").toBool(false));

    timeline_.append(row);
    while (timeline_.size() > timelineLimit_) {
        timeline_.removeAt(0);
    }

    QJsonArray correlated;
    for (const QJsonValue& value : timeline_) {
        const QJsonObject s = value.toObject();
        if (s.value("cpu_percent").toDouble() > 85.0
            && (s.value("orphan_topics").toInt() > 0 || s.value("tf_warnings").toInt() > 0)) {
            correlated.append(QJsonObject{
                {"timestamp_utc", s.value("timestamp_utc").toString()},
                {"inference", "CPU spike correlated with ROS degradation"},
            });
        }
    }
    return QJsonObject{{"timeline", timeline_}, {"correlated_events", correlated}};
}

QJsonObject DiagnosticsEngine::memoryLeakDetection(const QJsonArray& processes) {
    QSet<QString> active;
    for (const QJsonValue& value : processes) {
        const QJsonObject proc = value.toObject();
        const QString node = proc.value("node_name").toString();
        if (!proc.value("is_ros").toBool() || node.isEmpty()) {
            continue;
        }
        active.insert(node);
        QVector<double> history = memoryHistoryByNode_.value(node);
        history.push_back(proc.value("memory_percent").toDouble());
        while (history.size() > 120) {
            history.pop_front();
        }
        memoryHistoryByNode_.insert(node, history);
    }
    for (const QString& node : memoryHistoryByNode_.keys()) {
        if (!active.contains(node)) {
            memoryHistoryByNode_.remove(node);
        }
    }

    QJsonArray leaks;
    for (auto it = memoryHistoryByNode_.constBegin(); it != memoryHistoryByNode_.constEnd(); ++it) {
        const QVector<double>& h = it.value();
        if (h.size() < 8) {
            continue;
        }
        const double m = slope(h);
        if (m > 0.03 && (h.back() - h.front()) > 1.5) {
            leaks.append(QJsonObject{{"node", it.key()}, {"slope", m}, {"delta_percent", h.back() - h.front()}});
        }
    }
    return QJsonObject{{"leak_candidates", leaks}, {"candidate_count", leaks.size()}};
}

QJsonObject DiagnosticsEngine::ddsParticipantInspector(
    const QJsonArray& domains,
    const QJsonObject& health) {
    QJsonArray participants;
    QJsonArray storms;
    for (const QJsonValue& value : domains) {
        const QJsonObject domain = value.toObject();
        const QString id = domain.value("domain_id").toString("0");
        const int count = domain.value("ros_process_count").toInt();
        const int prev = previousParticipantsByDomain_.value(id, count);
        if (std::abs(count - prev) >= 8) {
            storms.append(QJsonObject{{"domain_id", id}, {"previous", prev}, {"current", count}});
        }
        previousParticipantsByDomain_.insert(id, count);
        participants.append(QJsonObject{{"domain_id", id}, {"participant_count", count}});
    }
    return QJsonObject{
        {"participants", participants},
        {"ghost_participants", health.value("zombie_nodes").toArray().size()},
        {"discovery_storms", storms},
    };
}

QJsonObject DiagnosticsEngine::networkSaturationMonitor(const QJsonObject& system, int pollIntervalMs) {
    const double dt = std::max(0.5, pollIntervalMs / 1000.0);
    const double alertMbps = expectedProfile_.value("network_alert_mbps").toDouble(250.0);
    QJsonArray ifaceRates;
    QJsonArray congested;

    for (const QJsonValue& value : system.value("network_interfaces").toArray()) {
        const QJsonObject iface = value.toObject();
        const QString name = iface.value("name").toString();
        const qint64 rx = static_cast<qint64>(iface.value("rx_bytes").toDouble(0));
        const qint64 tx = static_cast<qint64>(iface.value("tx_bytes").toDouble(0));
        const qint64 prevRx = previousRxBytesByIface_.value(name, rx);
        const qint64 prevTx = previousTxBytesByIface_.value(name, tx);
        previousRxBytesByIface_.insert(name, rx);
        previousTxBytesByIface_.insert(name, tx);

        const double mbps = bpsToMbps((std::max<qint64>(0, rx - prevRx) + std::max<qint64>(0, tx - prevTx)) / dt);
        QJsonObject row{{"interface", name}, {"total_mbps", mbps}};
        ifaceRates.append(row);
        if (mbps > alertMbps) {
            congested.append(row);
        }
    }

    QJsonArray highTrafficTopics;
    for (auto it = lastTopicBandwidthByTopic_.constBegin(); it != lastTopicBandwidthByTopic_.constEnd(); ++it) {
        const double mbps = bpsToMbps(it.value());
        if (mbps > 30.0) {
            highTrafficTopics.append(QJsonObject{{"topic", it.key()}, {"throughput_mbps", mbps}});
        }
    }

    return QJsonObject{
        {"interface_rates", ifaceRates},
        {"congested_interfaces", congested},
        {"high_traffic_publishers", highTrafficTopics},
    };
}

QJsonObject DiagnosticsEngine::softSafetyBoundary(
    const QJsonObject& tfNav2,
    const QJsonObject& topicRates) {
    QHash<QString, double> hzByTopic;
    for (const QJsonValue& value : topicRates.value("topic_metrics").toArray()) {
        const QJsonObject row = value.toObject();
        hzByTopic.insert(row.value("topic").toString(), row.value("actual_hz").toDouble(-1));
    }

    QJsonArray warnings;
    if (hzByTopic.contains("/local_costmap/costmap") && hzByTopic.value("/local_costmap/costmap") < 1.0) {
        warnings.append("Costmap update rate is below threshold.");
    }
    if (hzByTopic.contains("/imu") && hzByTopic.value("/imu") >= 0.0 && hzByTopic.value("/imu") < 5.0) {
        warnings.append("IMU stream appears degraded or stalled.");
    }
    if (!tfNav2.value("tf_warnings").toArray().isEmpty()) {
        warnings.append("TF integrity warnings detected.");
    }
    return QJsonObject{{"warnings", warnings}, {"warning_count", warnings.size()}};
}

QJsonObject DiagnosticsEngine::workspaceTools(const QJsonArray& processes) const {
    QSet<QString> workspaces;
    QHash<QString, QSet<QString>> packageMap;
    QSet<QString> distros;

    for (const QJsonValue& value : processes) {
        const QJsonObject proc = value.toObject();
        if (!proc.value("is_ros").toBool()) {
            continue;
        }
        const QString ws = proc.value("workspace_origin").toString();
        const QString pkg = proc.value("package").toString();
        if (!ws.isEmpty()) {
            workspaces.insert(ws);
        }
        if (!ws.isEmpty() && !pkg.isEmpty()) {
            packageMap[pkg].insert(ws);
        }

        QRegularExpression re("/opt/ros/([^/]+)");
        QRegularExpressionMatch m = re.match(ws);
        if (m.hasMatch()) {
            distros.insert(m.captured(1));
        }
    }

    QJsonArray duplicatePackages;
    for (auto it = packageMap.constBegin(); it != packageMap.constEnd(); ++it) {
        if (it.value().size() > 1) {
            QStringList wsList = it.value().values();
            duplicatePackages.append(
                QJsonObject{
                    {"package", it.key()},
                    {"workspaces", QJsonArray::fromStringList(wsList)},
                });
        }
    }

    QStringList chainValues = workspaces.values();
    QJsonArray chain = QJsonArray::fromStringList(chainValues);
    QStringList distroValues = distros.values();
    return QJsonObject{
        {"overlay_chain", chain},
        {"duplicate_packages", duplicatePackages},
        {"mixed_ros_distributions", distros.size() > 1},
        {"detected_distributions", QJsonArray::fromStringList(distroValues)},
        {"abi_mismatch_suspected", distros.size() > 1},
    };
}

QJsonObject DiagnosticsEngine::actionMonitor(
    const QJsonObject& tfNav2,
    const QJsonObject& graph) const {
    int servers = 0;
    int clients = 0;
    for (const QJsonValue& nodeValue : graph.value("nodes").toArray()) {
        const QJsonObject node = nodeValue.toObject();
        servers += node.value("action_servers").toArray().size();
        clients += node.value("action_clients").toArray().size();
    }
    const bool goalActive = tfNav2.value("nav2").toObject().value("goal_active").toBool(false);
    return QJsonObject{
        {"active_goals", goalActive ? 1 : 0},
        {"action_servers", servers},
        {"action_clients", clients},
        {"failed_goals", 0},
        {"timeouts_suspected", clients > 0 && !goalActive},
    };
}

QJsonObject DiagnosticsEngine::tfDriftMonitor(const QJsonObject& tfNav2) const {
    QHash<QString, QSet<QString>> parentsByChild;
    for (const QJsonValue& edgeValue : tfNav2.value("tf_edges").toArray()) {
        const QJsonObject edge = edgeValue.toObject();
        parentsByChild[edge.value("child").toString()].insert(edge.value("parent").toString());
    }

    QJsonArray duplicates;
    for (auto it = parentsByChild.constBegin(); it != parentsByChild.constEnd(); ++it) {
        if (it.value().size() > 1) {
            duplicates.append(QJsonObject{{"child_frame", it.key()}, {"parent_count", it.value().size()}});
        }
    }
    return QJsonObject{
        {"duplicate_frame_broadcasters", duplicates},
        {"parent_child_mismatch_count", duplicates.size()},
        {"timestamp_offset_ms", -1},
    };
}

QJsonObject DiagnosticsEngine::runtimeFingerprint(
    const QJsonObject& graph,
    const QJsonObject& tfNav2,
    const QJsonObject& system) const {
    QStringList nodes;
    QStringList topics;
    QStringList tfEdges;
    for (const QJsonValue& value : graph.value("nodes").toArray()) {
        nodes.append(value.toObject().value("full_name").toString());
    }
    for (const QJsonValue& value : graph.value("topics").toArray()) {
        topics.append(value.toObject().value("topic").toString());
    }
    for (const QJsonValue& value : tfNav2.value("tf_edges").toArray()) {
        const QJsonObject e = value.toObject();
        tfEdges.append(e.value("parent").toString() + "->" + e.value("child").toString());
    }
    std::sort(nodes.begin(), nodes.end());
    std::sort(topics.begin(), topics.end());
    std::sort(tfEdges.begin(), tfEdges.end());

    const double cpu = std::round(system.value("cpu").toObject().value("usage_percent").toDouble() / 5.0) * 5.0;
    const QString payload = nodes.join("|") + "::" + topics.join("|") + "::" + tfEdges.join("|") + "::" + QString::number(cpu);
    return QJsonObject{
        {"signature", hashText(payload)},
        {"node_count", nodes.size()},
        {"topic_count", topics.size()},
        {"tf_edge_count", tfEdges.size()},
    };
}

QJsonObject DiagnosticsEngine::deterministicLaunchValidation(const QJsonObject& graph) const {
    QSet<QString> currentNodes;
    for (const QJsonValue& value : graph.value("nodes").toArray()) {
        currentNodes.insert(value.toObject().value("full_name").toString());
    }
    QSet<QString> expectedNodes;
    for (const QJsonValue& value : expectedProfile_.value("expected_nodes").toArray()) {
        expectedNodes.insert(value.toString());
    }

    QJsonArray rogue;
    QJsonArray missing;
    if (!expectedNodes.isEmpty()) {
        for (const QString& node : currentNodes.values()) {
            if (!expectedNodes.contains(node)) {
                rogue.append(node);
            }
        }
        for (const QString& node : expectedNodes.values()) {
            if (!currentNodes.contains(node)) {
                missing.append(node);
            }
        }
    }

    return QJsonObject{
        {"rogue_nodes", rogue},
        {"missing_nodes", missing},
        {"valid", rogue.isEmpty() && missing.isEmpty()},
    };
}

QJsonObject DiagnosticsEngine::dependencyImpactMap(const QJsonObject& graph) const {
    QHash<QString, QSet<QString>> adjacency;
    QSet<QString> nodes;
    for (const QJsonValue& topicValue : graph.value("topics").toArray()) {
        const QJsonObject topic = topicValue.toObject();
        QStringList pubs;
        QStringList subs;
        for (const QJsonValue& p : topic.value("publishers").toArray()) {
            pubs.append(p.toString());
        }
        for (const QJsonValue& s : topic.value("subscribers").toArray()) {
            subs.append(s.toString());
        }
        for (const QString& p : pubs) {
            nodes.insert(p);
            for (const QString& s : subs) {
                nodes.insert(s);
                adjacency[p].insert(s);
            }
        }
    }

    QVector<QJsonObject> scores;
    for (const QString& node : nodes.values()) {
        QSet<QString> visited;
        QList<QString> queue = {node};
        while (!queue.isEmpty()) {
            const QString cur = queue.takeFirst();
            for (const QString& child : adjacency.value(cur).values()) {
                if (!visited.contains(child)) {
                    visited.insert(child);
                    queue.append(child);
                }
            }
        }
        scores.push_back(QJsonObject{{"node", node}, {"downstream_count", visited.size()}});
    }
    std::sort(scores.begin(), scores.end(), [](const QJsonObject& a, const QJsonObject& b) {
        return a.value("downstream_count").toInt() > b.value("downstream_count").toInt();
    });
    QJsonArray scoreArray;
    for (const QJsonObject& score : scores) {
        scoreArray.append(score);
    }
    QJsonArray top;
    const int topCount = std::min(10, static_cast<int>(scores.size()));
    for (int i = 0; i < topCount; ++i) {
        top.append(scores[static_cast<qsizetype>(i)]);
    }
    return QJsonObject{{"impact_scores", scoreArray}, {"top_impact_nodes", top}};
}

int DiagnosticsEngine::runtimeStabilityScore(
    const QJsonObject& health,
    const QJsonObject& topicRates,
    const QJsonObject& memoryLeaks,
    const QJsonObject& network) {
    int score = 100;
    const QString status = health.value("status").toString("healthy");
    if (status == "critical") {
        score -= 40;
    } else if (status == "warning") {
        score -= 20;
    }
    score -= topicRates.value("dropped_topics").toArray().size() * 5;
    score -= memoryLeaks.value("candidate_count").toInt() * 6;
    score -= network.value("congested_interfaces").toArray().size() * 4;
    return std::max(0, std::min(100, score));
}

QString DiagnosticsEngine::stableHash(const QString& value) {
    return hashText(value);
}

double DiagnosticsEngine::parseAverageRate(const QString& text) {
    return parseAverageRateText(text);
}

double DiagnosticsEngine::parseAverageBandwidth(const QString& text) {
    return parseBandwidthBps(text);
}

double DiagnosticsEngine::linearSlope(const QVector<double>& values) {
    return slope(values);
}

}  // namespace rrcc
