#include "rrcc/health_monitor.hpp"

#include <QHash>
#include <QSet>

namespace rrcc {

QJsonObject HealthMonitor::evaluate(
    const QJsonArray& domains,
    const QJsonObject& graph,
    const QJsonObject& tfNav2) const {
    QJsonArray zombieNodes;
    QHash<QString, QSet<QString>> nodeDomains;

    for (const QJsonValue& domainValue : domains) {
        const QJsonObject domain = domainValue.toObject();
        const QString domainId = domain.value("domain_id").toString("0");
        for (const QJsonValue& nodeValue : domain.value("nodes").toArray()) {
            const QJsonObject node = nodeValue.toObject();
            const QString fullName = node.value("full_name").toString();
            nodeDomains[fullName].insert(domainId);
            if (node.value("pid").toInt(-1) < 0) {
                QJsonObject zombie;
                zombie.insert("domain_id", domainId);
                zombie.insert("node", fullName);
                zombieNodes.append(zombie);
            }
        }
    }

    QJsonArray domainConflicts;
    for (auto it = nodeDomains.constBegin(); it != nodeDomains.constEnd(); ++it) {
        if (it.value().size() > 1) {
            QJsonObject conflict;
            conflict.insert("node", it.key());
            QJsonArray conflictDomains;
            const QStringList values = it.value().values();
            for (const QString& domain : values) {
                conflictDomains.append(domain);
            }
            conflict.insert("domains", conflictDomains);
            domainConflicts.append(conflict);
        }
    }

    const QJsonArray duplicateNodes = graph.value("duplicate_node_names").toArray();
    const QJsonArray noSubscriberTopics = graph.value("publishers_without_subscribers").toArray();
    const QJsonArray noPublisherTopics = graph.value("subscribers_without_publishers").toArray();
    const QJsonArray missingServiceServers = graph.value("missing_service_servers").toArray();
    const QJsonArray missingActionServers = graph.value("missing_action_servers").toArray();
    const QJsonArray misinitializedProcesses = graph.value("misinitialized_processes").toArray();
    const QJsonArray tfWarnings = tfNav2.value("tf_warnings").toArray();

    const QJsonObject nav2 = tfNav2.value("nav2").toObject();
    const bool goalActive = nav2.value("goal_active").toBool(false);

    QString status = "healthy";
    if (!zombieNodes.isEmpty() || !domainConflicts.isEmpty() || !misinitializedProcesses.isEmpty()) {
        status = "critical";
    } else if (!duplicateNodes.isEmpty() || !tfWarnings.isEmpty()
        || !noSubscriberTopics.isEmpty() || !noPublisherTopics.isEmpty()
        || !missingServiceServers.isEmpty() || !missingActionServers.isEmpty()) {
        status = "warning";
    }

    QJsonObject out;
    out.insert("status", status);
    out.insert("duplicate_nodes", duplicateNodes);
    out.insert("zombie_nodes", zombieNodes);
    out.insert("domain_conflicts", domainConflicts);
    out.insert("publishers_without_subscribers", noSubscriberTopics);
    out.insert("subscribers_without_publishers", noPublisherTopics);
    out.insert("missing_service_servers", missingServiceServers);
    out.insert("missing_action_servers", missingActionServers);
    out.insert("misinitialized_processes", misinitializedProcesses);
    out.insert("tf_warnings", tfWarnings);
    out.insert("nav2_goal_active", goalActive);
    return out;
}

}  // namespace rrcc
