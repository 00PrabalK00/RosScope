#include "rrcc/runtime_worker.hpp"

#include <QDateTime>
#include <QElapsedTimer>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QFile>
#include <QCryptographicHash>
#include <QMap>
#include <QStringList>
#include <QThread>

#include "rrcc/command_runner.hpp"
#include "rrcc/telemetry.hpp"

namespace rrcc {

namespace {

QString compactHash(const QJsonValue& value) {
    const QByteArray payload = QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha1).toHex());
}

QString compactHashArray(const QJsonArray& value) {
    const QByteArray payload = QJsonDocument(value).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha1).toHex());
}

QString compactHashText(const QString& value) {
    return QString::fromLatin1(
        QCryptographicHash::hash(value.toUtf8(), QCryptographicHash::Sha1).toHex());
}

}  // namespace

RuntimeWorker::RuntimeWorker(QObject* parent)
    : QObject(parent),
      actions_(&processManager_) {
    const QString defaultPresetPath = QDir(QDir::currentPath()).filePath("presets/default.json");
    if (QFile::exists(defaultPresetPath)) {
        loadRuntimePreset("default");
    }
    const QString fleetPath = QDir(QDir::currentPath()).filePath("fleet_targets.json");
    if (QFile::exists(fleetPath)) {
        remoteMonitor_.loadTargetsFromFile(fleetPath);
    }
}

void RuntimeWorker::pruneParameterCache() {
    while (parameterCacheOrder_.size() > maxParameterCacheEntries_) {
        const QString oldest = parameterCacheOrder_.takeFirst();
        parameterCache_.remove(oldest);
    }
}

QJsonArray RuntimeWorker::applyProcessFilter(
    const QJsonArray& processes,
    bool rosOnly,
    const QString& query,
    const QString& scope) const {
    const QString queryLower = query.trimmed().toLower();
    const QString scopeLower = scope.trimmed().toLower();
    QJsonArray filtered;

    for (const QJsonValue& value : processes) {
        const QJsonObject proc = value.toObject();
        if (rosOnly && !proc.value("is_ros").toBool()) {
            continue;
        }
        if (scopeLower == "ros only" && !proc.value("is_ros").toBool()) {
            continue;
        }
        if (scopeLower.startsWith("domain ")) {
            const QString domain = scopeLower.mid(QString("domain ").size()).trimmed();
            if (proc.value("ros_domain_id").toString("0") != domain) {
                continue;
            }
        }

        if (!queryLower.isEmpty()) {
            const QString searchable =
                QString::number(proc.value("pid").toInt())
                + " "
                + proc.value("name").toString()
                + " "
                + proc.value("executable").toString()
                + " "
                + proc.value("command_line").toString();
            if (!searchable.toLower().contains(queryLower)) {
                continue;
            }
        }
        filtered.append(proc);
    }
    return filtered;
}

QJsonObject RuntimeWorker::buildResponse(
    const QString& selectedDomain,
    const QJsonArray& allProcesses,
    const QJsonArray& visibleProcesses,
    const QJsonArray& domainSummaries,
    const QJsonArray& domainDetails,
    const QJsonObject& graph,
    const QJsonObject& tfNav2,
    const QJsonObject& system,
    const QString& logs,
    const QJsonObject& health,
    const QJsonObject& advanced,
    const QJsonObject& fleet,
    const QJsonObject& session,
    const QJsonObject& watchdog) const {
    QJsonObject snapshot;
    snapshot.insert("timestamp_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    snapshot.insert("preset_name", presetName_);
    snapshot.insert("selected_domain", selectedDomain);
    snapshot.insert("processes_all", allProcesses);
    snapshot.insert("processes_visible", visibleProcesses);
    snapshot.insert("domain_summaries", domainSummaries);
    snapshot.insert("domains", domainDetails);
    snapshot.insert("graph", graph);
    snapshot.insert("tf_nav2", tfNav2);
    snapshot.insert("system", system);
    snapshot.insert("logs", logs);
    snapshot.insert("health", health);
    snapshot.insert("node_parameters", parameterCache_);
    snapshot.insert("advanced", advanced);
    snapshot.insert("fleet", fleet);
    snapshot.insert("session", session);
    snapshot.insert("watchdog", watchdog);
    snapshot.insert("sync_version", static_cast<double>(syncVersion_));
    snapshot.insert("process_offset", request_.value("process_offset").toInt(0));
    snapshot.insert("process_limit", request_.value("process_limit").toInt(400));
    return snapshot;
}

void RuntimeWorker::poll(const QJsonObject& request) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (lastPollEpochMs_ > 0 && (now - lastPollEpochMs_) < minPollIntervalMs_) {
        const int sleepMs = minPollIntervalMs_ - static_cast<int>(now - lastPollEpochMs_);
        if (sleepMs > 0) {
            QThread::msleep(static_cast<unsigned long>(sleepMs));
        }
    }
    request_ = request;
    if (busy_) {
        pending_ = true;
        return;
    }
    pollNow();
}

void RuntimeWorker::pollNow() {
    QElapsedTimer pollTimer;
    pollTimer.start();
    busy_ = true;
    pollCounter_++;
    lastPollEpochMs_ = QDateTime::currentMSecsSinceEpoch();
    Telemetry::instance().recordRequest();
    Telemetry::instance().incrementCounter("sync.poll_count");

    const bool rosOnly = request_.value("ros_only").toBool(false);
    const QString processQuery = request_.value("process_query").toString();
    const QString processScope = request_.value("process_scope").toString("ROS Only");
    const qint64 sinceVersion = request_.value("since_version").toInteger(-1);
    const int processOffset = qMax(0, request_.value("process_offset").toInt(0));
    const int processLimit = qBound(100, request_.value("process_limit").toInt(400), 2000);
    QString selectedDomain = request_.value("selected_domain").toString("0");
    const int activeTab = request_.value("active_tab").toInt(0);
    const bool engineerMode = request_.value("engineer_mode").toBool(true);

    const bool idleFastPath =
        consecutiveNoChangePolls_ >= 3
        && activeTab != 0
        && activeTab != 1
        && (pollCounter_ % 2 == 0)
        && !lastAllProcesses_.isEmpty();

    if (!idleFastPath) {
        const bool deepRosInspection = processScope.toLower() != "all processes";
        lastAllProcesses_ = processManager_.listProcesses(false, "", deepRosInspection);
        lastDomainSummaries_ = rosInspector_.listDomains(lastAllProcesses_);
    } else {
        Telemetry::instance().incrementCounter("sync.idle_fastpath_hits");
    }

    const QJsonArray filteredProcesses =
        applyProcessFilter(lastAllProcesses_, rosOnly, processQuery, processScope);
    QJsonArray pagedProcesses;
    const int end = qMin(filteredProcesses.size(), processOffset + processLimit);
    for (int i = processOffset; i < end; ++i) {
        pagedProcesses.append(filteredProcesses.at(i));
    }
    lastVisibleProcesses_ = pagedProcesses;

    QStringList knownDomains;
    for (const QJsonValue& value : lastDomainSummaries_) {
        knownDomains.append(value.toObject().value("domain_id").toString("0"));
    }

    if (selectedDomain.isEmpty() || !knownDomains.contains(selectedDomain)) {
        selectedDomain = knownDomains.isEmpty() ? "0" : knownDomains.first();
    }

    const bool refreshAllDomainDetails =
        activeTab == 1 || pollCounter_ % 4 == 0 || lastDomainDetails_.isEmpty();
    const bool refreshSelectedDomainDetail = activeTab == 2 || activeTab == 3;

    QHash<QString, QJsonObject> detailByDomain;
    for (const QJsonValue& value : lastDomainDetails_) {
        const QJsonObject detail = value.toObject();
        detailByDomain.insert(detail.value("domain_id").toString("0"), detail);
    }

    if (refreshAllDomainDetails) {
        detailByDomain.clear();
        for (const QString& domainId : knownDomains) {
            detailByDomain.insert(
                domainId, rosInspector_.inspectDomain(domainId, lastAllProcesses_, false));
        }
    } else if (refreshSelectedDomainDetail) {
        detailByDomain.insert(
            selectedDomain, rosInspector_.inspectDomain(selectedDomain, lastAllProcesses_, false));
    }

    QJsonArray domainDetails;
    for (const QJsonValue& summaryValue : lastDomainSummaries_) {
        const QJsonObject summary = summaryValue.toObject();
        const QString domainId = summary.value("domain_id").toString("0");

        QJsonObject detail = detailByDomain.value(domainId);
        if (detail.isEmpty()) {
            detail.insert("domain_id", domainId);
            detail.insert("nodes", QJsonArray{});
        }

        detail.insert("ros_process_count", summary.value("ros_process_count"));
        detail.insert("domain_cpu_percent", summary.value("domain_cpu_percent"));
        detail.insert("domain_memory_percent", summary.value("domain_memory_percent"));
        detail.insert("workspace_count", summary.value("workspace_count"));
        domainDetails.append(detail);
    }
    lastDomainDetails_ = domainDetails;

    // Heavy ROS graph probes are decimated unless the relevant tab is active.
    const bool needGraph =
        (engineerMode && (activeTab == 2 || activeTab == 6 || activeTab == 7 || activeTab == 8 || pollCounter_ % 4 == 0))
        || (!engineerMode && pollCounter_ % (idleBackoffMs_ >= 4000 ? 18 : 10) == 0);
    const bool needTf =
        (engineerMode && (activeTab == 3 || activeTab == 6 || activeTab == 7 || activeTab == 8 || pollCounter_ % 5 == 0))
        || (!engineerMode && pollCounter_ % (idleBackoffMs_ >= 4000 ? 24 : 15) == 0);
    const bool needLogs =
        (engineerMode && ((activeTab == 5) || pollCounter_ % 4 == 0))
        || (!engineerMode && pollCounter_ % (idleBackoffMs_ >= 4000 ? 16 : 8) == 0);

    if (needGraph || lastGraph_.isEmpty() || lastGraph_.value("domain_id").toString() != selectedDomain) {
        lastGraph_ = rosInspector_.inspectGraph(selectedDomain, lastAllProcesses_);
    }
    if (needTf || lastTfNav2_.isEmpty() || lastTfNav2_.value("domain_id").toString() != selectedDomain) {
        lastTfNav2_ = rosInspector_.inspectTfNav2(selectedDomain);
    }

    lastSystem_ = systemMonitor_.collectSystem();
    if (needLogs || lastLogs_.isEmpty()) {
        lastLogs_ = systemMonitor_.tailDmesg(300);
    }

    lastHealth_ = healthMonitor_.evaluate(lastDomainDetails_, lastGraph_, lastTfNav2_);

    const bool deepSampling =
        engineerMode && (activeTab == 2 || activeTab == 3 || activeTab == 6 || activeTab == 7
        || activeTab == 8 || pollCounter_ % 3 == 0);
    lastAdvanced_ = diagnosticsEngine_.evaluate(
        selectedDomain,
        lastAllProcesses_,
        lastDomainDetails_,
        lastGraph_,
        lastTfNav2_,
        lastSystem_,
        lastHealth_,
        parameterCache_,
        deepSampling,
        2000);

    if (watchdogEnabled_) {
        applyWatchdog(selectedDomain);
    }

    if (activeTab == 10 || pollCounter_ % 8 == 0) {
        lastFleet_ = remoteMonitor_.collectFleetStatus(4500);
    }
    if (pollCounter_ % 6 == 0) {
        remoteMonitor_.resumeQueuedActions(2, 4500);
    }

    lastWatchdog_ = {
        {"enabled", watchdogEnabled_},
        {"last_action_epoch_ms", lastWatchdogActionMs_},
        {"soft_boundary_warnings",
         lastAdvanced_.value("soft_safety_boundary").toObject().value("warning_count").toInt()},
    };

    QJsonObject response = buildResponse(
        selectedDomain,
        lastAllProcesses_,
        lastVisibleProcesses_,
        lastDomainSummaries_,
        lastDomainDetails_,
        lastGraph_,
        lastTfNav2_,
        lastSystem_,
        lastLogs_,
        lastHealth_,
        lastAdvanced_,
        lastFleet_,
        sessionRecorder_.status(),
        lastWatchdog_);
    response.insert("process_total_filtered", filteredProcesses.size());
    response.insert("process_offset", processOffset);
    response.insert("process_limit", processLimit);

    const QJsonObject sectionHashes = {
        {"processes_visible", compactHashArray(lastVisibleProcesses_)},
        {"domain_summaries", compactHashArray(lastDomainSummaries_)},
        {"domains", compactHashArray(lastDomainDetails_)},
        {"graph", compactHash(lastGraph_)},
        {"tf_nav2", compactHash(lastTfNav2_)},
        {"system", compactHash(lastSystem_)},
        {"health", compactHash(lastHealth_)},
        {"advanced", compactHash(lastAdvanced_)},
        {"fleet", compactHash(lastFleet_)},
        {"session", compactHash(sessionRecorder_.status())},
        {"watchdog", compactHash(lastWatchdog_)},
        {"logs", compactHashText(lastLogs_)},
    };
    const QString fingerprint = compactHash(sectionHashes);
    const bool changed = (fingerprint != lastSyncFingerprint_);
    if (changed) {
        syncVersion_++;
        lastSyncFingerprint_ = fingerprint;
        consecutiveNoChangePolls_ = 0;
        idleBackoffMs_ = 1000;
    } else {
        consecutiveNoChangePolls_++;
        idleBackoffMs_ = qMin(maxBackoffMs_, idleBackoffMs_ * 2);
    }
    response.insert("sync_version", static_cast<double>(syncVersion_));
    response.insert("etag", fingerprint);
    response.insert("changed", changed);
    response.insert("changed_sections", sectionHashes);
    response.insert("idle_backoff_ms", idleBackoffMs_);
    response.insert("offline_queue_size", lastFleet_.value("offline_queue_size"));

    if (!changed && sinceVersion == syncVersion_) {
        response.remove("processes_all");
        response.remove("processes_visible");
        response.remove("domain_summaries");
        response.remove("domains");
        response.remove("graph");
        response.remove("tf_nav2");
        response.remove("system");
        response.remove("logs");
        response.remove("health");
        response.remove("advanced");
        response.remove("fleet");
        response.remove("session");
        response.remove("watchdog");
        response.remove("node_parameters");
        response.insert("heartbeat_only", true);
    }

    penultimateSnapshot_ = previousSnapshot_;
    previousSnapshot_ = response;
    sessionRecorder_.recordSample(response);

    emit snapshotReady(response);
    Telemetry::instance().recordDurationMs("sync.duration_ms", pollTimer.elapsed());
    Telemetry::instance().setGauge("sync.idle_backoff_ms", idleBackoffMs_);
    Telemetry::instance().setGauge("sync.consecutive_no_change", consecutiveNoChangePolls_);

    busy_ = false;
    if (pending_) {
        pending_ = false;
        pollNow();
    }
}

void RuntimeWorker::runAction(const QString& action, const QJsonObject& payload) {
    QElapsedTimer actionTimer;
    actionTimer.start();
    Telemetry::instance().incrementCounter("actions.count");
    QJsonObject result;
    result.insert("action", action);
    result.insert("success", false);

    if (action == "terminate_pid") {
        const qint64 pid = static_cast<qint64>(payload.value("pid").toDouble(-1));
        const bool ok = processManager_.terminateProcess(pid);
        result.insert("success", ok);
        result.insert("message", ok ? QString("SIGTERM sent to %1").arg(pid)
                                    : QString("Failed to SIGTERM %1").arg(pid));
    } else if (action == "kill_pid") {
        const qint64 pid = static_cast<qint64>(payload.value("pid").toDouble(-1));
        const bool ok = processManager_.forceKillProcess(pid);
        result.insert("success", ok);
        result.insert("message", ok ? QString("SIGKILL sent to %1").arg(pid)
                                    : QString("Failed to SIGKILL %1").arg(pid));
    } else if (action == "kill_tree") {
        const qint64 pid = static_cast<qint64>(payload.value("pid").toDouble(-1));
        const bool ok = processManager_.killProcessTree(pid, true);
        result.insert("success", ok);
        result.insert("message", ok ? QString("Killed process tree for %1").arg(pid)
                                    : QString("Failed killing process tree for %1").arg(pid));
    } else if (action == "kill_all_ros") {
        result = actions_.killAllRosProcesses(lastAllProcesses_);
        result.insert(
            "message",
            QString("Killed %1 ROS processes, %2 failed.")
                .arg(result.value("killed_count").toInt())
                .arg(result.value("failed_count").toInt()));
    } else if (action == "restart_domain") {
        result = actions_.restartDomain(payload.value("domain_id").toString("0"), lastAllProcesses_);
        result.insert(
            "message",
            QString("Domain %1 restart: %2 terminated.")
                .arg(payload.value("domain_id").toString("0"))
                .arg(result.value("terminated_processes").toInt()));
    } else if (action == "clear_shared_memory") {
        result = actions_.clearSharedMemory();
        result.insert("message", "Shared memory cleanup executed.");
    } else if (action == "restart_workspace") {
        result = actions_.restartWorkspace(
            payload.value("workspace_path").toString(),
            payload.value("relaunch_command").toString(),
            lastAllProcesses_);
        result.insert(
            "message",
            QString("Workspace restart: %1 terminated.")
                .arg(result.value("terminated_processes").toInt()));
    } else if (action == "snapshot_json" || action == "snapshot_yaml") {
        const QString format = (action == "snapshot_yaml") ? "yaml" : "json";
        const QString graphDomain = lastGraph_.value("domain_id").toString("0");

        // Snapshot action captures parameters for visible graph nodes on demand.
        QJsonObject snapshotParams = parameterCache_;
        for (const QJsonValue& nodeValue : lastGraph_.value("nodes").toArray()) {
            const QString nodeName = nodeValue.toObject().value("full_name").toString();
            if (nodeName.isEmpty() || snapshotParams.contains(nodeName)) {
                continue;
            }
            const QJsonObject params = rosInspector_.fetchNodeParameters(graphDomain, nodeName);
            if (params.value("success").toBool(false)) {
                snapshotParams.insert(nodeName, params.value("parameters").toString());
                parameterCacheOrder_.append(nodeName);
                parameterCacheOrder_.removeDuplicates();
            }
        }
        parameterCache_ = snapshotParams;
        pruneParameterCache();

        const QJsonObject snapshot = snapshotManager_.buildSnapshot(
            lastAllProcesses_,
            lastDomainDetails_,
            lastGraph_,
            lastTfNav2_,
            lastSystem_,
            lastHealth_,
            snapshotParams);
        QJsonObject enriched = snapshot;
        enriched.insert("advanced", lastAdvanced_);
        enriched.insert("fleet", lastFleet_);
        enriched.insert("session", sessionRecorder_.status());
        enriched.insert("watchdog", lastWatchdog_);
        enriched.insert("preset_name", presetName_);
        result = snapshotManager_.exportSnapshot(enriched, format);
        result.insert("action", action);
    } else if (action == "compare_snapshots") {
        result = snapshotDiff_.compareFiles(
            payload.value("left_path").toString(),
            payload.value("right_path").toString());
        result.insert("action", action);
    } else if (action == "compare_with_previous") {
        if (penultimateSnapshot_.isEmpty()) {
            result.insert("success", false);
            result.insert("error", "No previous snapshot available for diff.");
        } else {
            result = snapshotDiff_.compare(penultimateSnapshot_, buildResponse(
                lastGraph_.value("domain_id").toString("0"),
                lastAllProcesses_,
                lastVisibleProcesses_,
                lastDomainSummaries_,
                lastDomainDetails_,
                lastGraph_,
                lastTfNav2_,
                lastSystem_,
                lastLogs_,
                lastHealth_,
                lastAdvanced_,
                lastFleet_,
                sessionRecorder_.status(),
                lastWatchdog_));
            result.insert("success", true);
        }
        result.insert("action", action);
    } else if (action == "session_start") {
        result = sessionRecorder_.start(payload.value("session_name").toString("runtime_session"));
        result.insert("success", true);
        result.insert("action", action);
    } else if (action == "session_stop") {
        result = sessionRecorder_.stop();
        result.insert("success", true);
        result.insert("action", action);
    } else if (action == "session_export") {
        result = sessionRecorder_.exportSession(payload.value("format").toString("json"));
        result.insert("action", action);
    } else if (action == "export_telemetry") {
        const QString path = payload.value("path").toString(
            QDir(QDir::currentPath()).filePath("logs/telemetry.json"));
        result = Telemetry::instance().exportToFile(path);
        result.insert("action", action);
    } else if (action == "save_preset") {
        result = saveRuntimePreset(payload.value("name").toString("default"));
        result.insert("action", action);
    } else if (action == "load_preset") {
        result = loadRuntimePreset(payload.value("name").toString("default"));
        result.insert("action", action);
    } else if (action == "watchdog_enable") {
        watchdogEnabled_ = true;
        result.insert("success", true);
        result.insert("message", "Watchdog enabled.");
    } else if (action == "watchdog_disable") {
        watchdogEnabled_ = false;
        result.insert("success", true);
        result.insert("message", "Watchdog disabled.");
    } else if (action == "isolate_domain") {
        const QString domainId = payload.value("domain_id").toString("0");
        int killed = 0;
        int failed = 0;
        for (const QJsonValue& value : lastAllProcesses_) {
            const QJsonObject proc = value.toObject();
            if (!proc.value("is_ros").toBool()) {
                continue;
            }
            if (proc.value("ros_domain_id").toString("0") != domainId) {
                continue;
            }
            const qint64 pid = static_cast<qint64>(proc.value("pid").toDouble(-1));
            if (pid <= 0) {
                continue;
            }
            if (processManager_.killProcessTree(pid, true)) {
                killed++;
            } else {
                failed++;
            }
        }
        const QMap<QString, QString> env = {{"ROS_DOMAIN_ID", domainId}};
        const CommandResult daemonStop = CommandRunner::run("ros2", {"daemon", "stop"}, 3000, env);
        result.insert("success", failed == 0);
        result.insert("killed_count", killed);
        result.insert("failed_count", failed);
        result.insert("daemon_stop_ok", daemonStop.success());
        result.insert("message", QString("Domain %1 isolated: %2 killed, %3 failed.")
                                     .arg(domainId)
                                     .arg(killed)
                                     .arg(failed));
    } else if (action == "fleet_load_targets") {
        result = remoteMonitor_.loadTargetsFromFile(payload.value("path").toString("fleet_targets.json"));
        result.insert("action", action);
    } else if (action == "fleet_refresh") {
        lastFleet_ = remoteMonitor_.collectFleetStatus(4500);
        result.insert("success", true);
        result.insert("fleet", lastFleet_);
        result.insert("message", "Fleet refresh complete.");
    } else if (action == "remote_action") {
        result = remoteMonitor_.executeRemoteAction(
            payload.value("target").toString(),
            payload.value("remote_action").toString(),
            payload.value("domain_id").toString("0"),
            4500);
        lastFleet_ = remoteMonitor_.collectFleetStatus(4500);
        result.insert("fleet", lastFleet_);
        result.insert("action", action);
    } else {
        result.insert("message", "Unsupported action");
    }

    emit actionFinished(result);
    Telemetry::instance().recordDurationMs("actions.duration_ms", actionTimer.elapsed());
    if (!result.value("success").toBool(false)) {
        Telemetry::instance().incrementCounter("actions.failures");
    }

    // Controls mutate runtime state; schedule fast refresh with existing request.
    if (action != "snapshot_json" && action != "snapshot_yaml"
        && action != "compare_snapshots" && action != "compare_with_previous"
        && action != "session_export") {
        poll(request_);
    }
}

void RuntimeWorker::fetchNodeParameters(const QString& domainId, const QString& nodeName) {
    QJsonObject result = rosInspector_.fetchNodeParameters(domainId, nodeName);
    if (result.value("success").toBool(false)) {
        parameterCache_.insert(nodeName, result.value("parameters").toString());
        parameterCacheOrder_.append(nodeName);
        parameterCacheOrder_.removeDuplicates();
        pruneParameterCache();
    }
    emit nodeParametersReady(result);
}

void RuntimeWorker::applyWatchdog(const QString& selectedDomain) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - lastWatchdogActionMs_ < 12000) {
        return;
    }

    const QString healthStatus = lastHealth_.value("status").toString("healthy");
    const int softWarnings =
        lastAdvanced_.value("soft_safety_boundary").toObject().value("warning_count").toInt();
    const int zombieCount = lastHealth_.value("zombie_nodes").toArray().size();
    const double cpu = lastSystem_.value("cpu").toObject().value("usage_percent").toDouble();

    bool actionTaken = false;
    QString actionMessage;
    if (zombieCount > 0) {
        QJsonObject result = actions_.restartDomain(selectedDomain, lastAllProcesses_);
        actionTaken = result.value("success").toBool(false);
        actionMessage = QString("Watchdog restart domain %1 (%2 zombies)").arg(selectedDomain).arg(zombieCount);
    } else if (cpu > 95.0 || healthStatus == "critical") {
        QJsonObject result = actions_.killAllRosProcesses(lastAllProcesses_);
        actionTaken = result.value("success").toBool(false);
        actionMessage = "Watchdog emergency stop due to critical load";
    } else if (softWarnings >= 4) {
        actionTaken = true;
        actionMessage = "Watchdog warning escalation without kill action";
    }

    if (actionTaken) {
        lastWatchdogActionMs_ = now;
        lastWatchdog_.insert("last_action_message", actionMessage);
    }
}

QJsonObject RuntimeWorker::saveRuntimePreset(const QString& name) const {
    QDir dir(QDir::currentPath());
    if (!dir.exists("presets")) {
        dir.mkpath("presets");
    }
    const QString preset = name.trimmed().isEmpty() ? "default" : name.trimmed();
    const QString path = dir.filePath(QString("presets/%1.json").arg(preset));

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return {
            {"success", false},
            {"error", "Failed to open preset file for writing."},
            {"path", path},
        };
    }

    QJsonObject payload;
    payload.insert("preset_name", preset);
    payload.insert("selected_domain", lastGraph_.value("domain_id").toString("0"));
    payload.insert("watchdog_enabled", watchdogEnabled_);
    payload.insert("expected_profile", diagnosticsEngine_.expectedProfile());
    payload.insert("remote_targets", remoteMonitor_.targets());
    payload.insert("timestamp_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    file.write(QJsonDocument(payload).toJson(QJsonDocument::Indented));
    file.close();

    return {
        {"success", true},
        {"path", path},
        {"preset_name", preset},
    };
}

QJsonObject RuntimeWorker::loadRuntimePreset(const QString& name) {
    const QString preset = name.trimmed().isEmpty() ? "default" : name.trimmed();
    const QString path = QDir(QDir::currentPath()).filePath(QString("presets/%1.json").arg(preset));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {
            {"success", false},
            {"error", "Failed to read preset file."},
            {"path", path},
        };
    }

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) {
        return {
            {"success", false},
            {"error", "Preset file is not a valid JSON object."},
            {"path", path},
        };
    }
    const QJsonObject payload = doc.object();
    diagnosticsEngine_.setExpectedProfile(payload.value("expected_profile").toObject());
    remoteMonitor_.setTargets(payload.value("remote_targets").toArray());
    watchdogEnabled_ = payload.value("watchdog_enabled").toBool(false);
    presetName_ = payload.value("preset_name").toString(preset);

    return {
        {"success", true},
        {"preset_name", presetName_},
        {"selected_domain", payload.value("selected_domain").toString("0")},
    };
}

}  // namespace rrcc
