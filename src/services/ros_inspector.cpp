#include "rrcc/ros_inspector.hpp"

#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <functional>

#include "rrcc/command_runner.hpp"

namespace rrcc {

namespace {

QJsonArray toJsonArray(const QSet<QString>& values) {
    QStringList list = values.values();
    std::sort(list.begin(), list.end());
    return QJsonArray::fromStringList(list);
}

QString cleanGraphEntryLine(const QString& value) {
    QString line = value.trimmed();
    if (line.startsWith("*")) {
        line = line.mid(1).trimmed();
    }
    if (line.startsWith("-")) {
        line = line.mid(1).trimmed();
    }
    return line;
}

QString parseLifecycleStateText(const QString& text) {
    const QStringList lines = text.split('\n', Qt::SkipEmptyParts);
    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        const int idx = line.indexOf(':');
        if (idx > 0 && line.left(idx).toLower().contains("state")) {
            return line.mid(idx + 1).trimmed();
        }
        return line;
    }
    return {};
}

bool isPluginLikeParameter(const QString& parameterName) {
    const QString lower = parameterName.toLower();
    return lower.contains("plugin")
        || lower.contains("plugins")
        || lower.contains("library")
        || lower.contains("libraries")
        || lower.contains("class")
        || lower.contains("type");
}

QJsonArray parseTopicListWithTypes(const QString& text) {
    QJsonArray result;
    const QStringList lines = text.split('\n', Qt::SkipEmptyParts);
    QRegularExpression re("^\\s*([^\\s]+)\\s*\\[([^\\]]+)\\]\\s*$");
    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        QRegularExpressionMatch match = re.match(line);
        if (!match.hasMatch()) {
            continue;
        }
        QJsonObject row;
        row.insert("topic", match.captured(1).trimmed());
        row.insert("type", match.captured(2).trimmed());
        result.append(row);
    }
    return result;
}

QStringList inferBehaviorRoles(const QJsonObject& node) {
    QSet<QString> roles;

    const QJsonArray publishers = node.value("publishers").toArray();
    const QJsonArray subscribers = node.value("subscribers").toArray();
    const QJsonArray actionServers = node.value("action_servers").toArray();
    const QJsonArray actionClients = node.value("action_clients").toArray();
    const QJsonArray serviceServers = node.value("service_servers").toArray();
    const QJsonArray serviceClients = node.value("service_clients").toArray();

    for (const QJsonValue& value : publishers) {
        const QJsonObject pub = value.toObject();
        const QString topic = pub.value("name").toString();
        const QString type = pub.value("type").toString().toLower();

        if (type.contains("geometry_msgs/msg/twist")) {
            roles.insert("controller");
        }
        if (type.contains("nav_msgs/msg/path")) {
            roles.insert("planner");
        }
        if (type.contains("sensor_msgs/msg/image")) {
            roles.insert("perception");
        }
        if (type.contains("sensor_msgs/msg/pointcloud2")) {
            roles.insert("lidar_pipeline");
        }
        if (type.contains("tf2_msgs/msg/tfmessage") || topic == "/tf" || topic == "/tf_static") {
            roles.insert("state_estimation");
            roles.insert("transform_broadcaster");
        }
    }

    for (const QJsonValue& value : subscribers) {
        const QString type = value.toObject().value("type").toString().toLower();
        if (type.contains("sensor_msgs/msg/image") || type.contains("sensor_msgs/msg/pointcloud2")) {
            roles.insert("perception");
        }
    }

    if (!actionServers.isEmpty() || !actionClients.isEmpty()) {
        roles.insert("task_executor");
    }
    if (!serviceServers.isEmpty() || !serviceClients.isEmpty()) {
        roles.insert("service_oriented");
    }

    if (roles.isEmpty()) {
        roles.insert("generic");
    }

    QStringList ordered = roles.values();
    std::sort(ordered.begin(), ordered.end());
    return ordered;
}

}  // namespace

bool RosInspector::isRos2Available() const {
    if (!ros2Checked_) {
        const CommandResult check =
            CommandRunner::runShell("command -v ros2 >/dev/null 2>&1 && echo OK", 2000);
        ros2Available_ = check.stdoutText.contains("OK");
        ros2Checked_ = true;
    }
    return ros2Available_;
}

QMap<QString, QString> RosInspector::rosEnv(const QString& domainId) {
    return {{"ROS_DOMAIN_ID", domainId}};
}

QStringList RosInspector::parseLines(const QString& text) {
    QStringList lines;
    for (const QString& line : text.split('\n', Qt::SkipEmptyParts)) {
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty()) {
            lines.append(trimmed);
        }
    }
    return lines;
}

QString RosInspector::baseNodeName(const QString& fullName) {
    if (fullName.isEmpty()) {
        return {};
    }
    const int idx = fullName.lastIndexOf('/');
    if (idx < 0) {
        return fullName;
    }
    return fullName.mid(idx + 1);
}

QString RosInspector::nodeNamespace(const QString& fullName) {
    if (!fullName.startsWith('/')) {
        return "/";
    }
    const int idx = fullName.lastIndexOf('/');
    if (idx <= 0) {
        return "/";
    }
    return fullName.left(idx);
}

QJsonObject RosInspector::findProcessForNode(
    const QString& fullNodeName,
    const QJsonArray& processes) {
    const QString node = baseNodeName(fullNodeName);
    const QString ns = nodeNamespace(fullNodeName);

    for (const QJsonValue& value : processes) {
        const QJsonObject proc = value.toObject();
        if (!proc.value("is_ros").toBool()) {
            continue;
        }

        const QString procNode = proc.value("node_name").toString();
        const QString procNs = proc.value("namespace").toString("/");
        const QString commandLine = proc.value("command_line").toString();

        if (!procNode.isEmpty() && procNode == node && (procNs == ns || procNs == "/" || ns == "/")) {
            return proc;
        }
        if (commandLine.contains(fullNodeName) || commandLine.contains(QString("__node:=%1").arg(node))) {
            return proc;
        }
    }
    return {};
}

QJsonObject RosInspector::parseNodeInfoText(const QString& nodeInfoText) {
    QJsonArray publishers;
    QJsonArray subscribers;
    QJsonArray serviceServers;
    QJsonArray serviceClients;
    QJsonArray actionServers;
    QJsonArray actionClients;

    QJsonArray* currentArray = nullptr;

    for (const QString& rawLine : nodeInfoText.split('\n')) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        if (line == "Publishers:") {
            currentArray = &publishers;
            continue;
        }
        if (line == "Subscribers:") {
            currentArray = &subscribers;
            continue;
        }
        if (line == "Service Servers:") {
            currentArray = &serviceServers;
            continue;
        }
        if (line == "Service Clients:") {
            currentArray = &serviceClients;
            continue;
        }
        if (line == "Action Servers:") {
            currentArray = &actionServers;
            continue;
        }
        if (line == "Action Clients:") {
            currentArray = &actionClients;
            continue;
        }
        if (line.startsWith("Node name:")) {
            continue;
        }

        if (currentArray != nullptr) {
            QString entry = cleanGraphEntryLine(line);
            QString name = entry;
            QString type;
            const int colon = entry.lastIndexOf(':');
            if (colon > 0) {
                name = entry.left(colon).trimmed();
                type = entry.mid(colon + 1).trimmed();
            }
            QJsonObject item;
            item.insert("name", name);
            item.insert("type", type);
            currentArray->append(item);
        }
    }

    QJsonObject parsed;
    parsed.insert("publishers", publishers);
    parsed.insert("subscribers", subscribers);
    parsed.insert("service_servers", serviceServers);
    parsed.insert("service_clients", serviceClients);
    parsed.insert("action_servers", actionServers);
    parsed.insert("action_clients", actionClients);
    return parsed;
}

QJsonObject RosInspector::parseTopicInfoVerbose(const QString& topicInfoText) {
    QJsonObject out;
    out.insert("raw", topicInfoText.left(4096));

    int pubCount = 0;
    int subCount = 0;
    QJsonArray qosProfiles;

    QString reliability;
    QString durability;
    QString history;

    for (const QString& line : topicInfoText.split('\n', Qt::SkipEmptyParts)) {
        const QString trimmed = line.trimmed();
        if (trimmed.startsWith("Publisher count:")) {
            pubCount = trimmed.section(':', 1).trimmed().toInt();
        } else if (trimmed.startsWith("Subscription count:")) {
            subCount = trimmed.section(':', 1).trimmed().toInt();
        } else if (trimmed.startsWith("Reliability:")) {
            reliability = trimmed.section(':', 1).trimmed();
        } else if (trimmed.startsWith("Durability:")) {
            durability = trimmed.section(':', 1).trimmed();
        } else if (trimmed.startsWith("History (Depth):")) {
            history = trimmed.section(':', 1).trimmed();
            QJsonObject qos;
            qos.insert("reliability", reliability);
            qos.insert("durability", durability);
            qos.insert("history_depth", history);
            qosProfiles.append(qos);
            reliability.clear();
            durability.clear();
            history.clear();
        }
    }

    out.insert("publisher_count", pubCount);
    out.insert("subscription_count", subCount);
    out.insert("qos_profiles", qosProfiles);
    return out;
}

QJsonArray RosInspector::parseTfEdges(const QString& tfEchoText) {
    QJsonArray edges;
    QString parent;

    for (const QString& line : tfEchoText.split('\n')) {
        const QString trimmed = line.trimmed();
        if (trimmed.startsWith("frame_id:")) {
            parent = trimmed.section(':', 1).trimmed().remove('"');
        } else if (trimmed.startsWith("child_frame_id:")) {
            const QString child = trimmed.section(':', 1).trimmed().remove('"');
            if (!parent.isEmpty() && !child.isEmpty()) {
                QJsonObject edge;
                edge.insert("parent", parent);
                edge.insert("child", child);
                edges.append(edge);
            }
        }
    }
    return edges;
}
QJsonArray RosInspector::listDomains(const QJsonArray& processes) const {
    QSet<QString> domains;
    QHash<QString, int> rosCount;
    QHash<QString, int> participantsByDomain;
    QHash<QString, double> cpuByDomain;
    QHash<QString, double> memByDomain;
    QHash<QString, QSet<QString>> workspacesByDomain;

    domains.insert("0");
    for (const QJsonValue& value : processes) {
        const QJsonObject proc = value.toObject();
        if (!proc.value("is_ros").toBool()) {
            continue;
        }
        const QString domain = proc.value("ros_domain_id").toString("0");
        const QString workspace = proc.value("workspace_origin").toString();

        domains.insert(domain);
        rosCount[domain] += 1;
        participantsByDomain[domain] += 1;
        cpuByDomain[domain] += proc.value("cpu_percent").toDouble();
        memByDomain[domain] += proc.value("memory_percent").toDouble();
        if (!workspace.isEmpty()) {
            workspacesByDomain[domain].insert(workspace);
        }
    }

    QList<QString> ordered = domains.values();
    std::sort(ordered.begin(), ordered.end(), [](const QString& a, const QString& b) {
        bool okA = false;
        bool okB = false;
        const int ai = a.toInt(&okA);
        const int bi = b.toInt(&okB);
        if (okA && okB) {
            return ai < bi;
        }
        return a < b;
    });

    QJsonArray result;
    for (const QString& domain : ordered) {
        QJsonObject domainObj;
        domainObj.insert("domain_id", domain);
        domainObj.insert("ros_process_count", rosCount.value(domain));
        domainObj.insert("dds_participant_count", participantsByDomain.value(domain));
        domainObj.insert("domain_cpu_percent", cpuByDomain.value(domain));
        domainObj.insert("domain_memory_percent", memByDomain.value(domain));
        domainObj.insert("workspace_count", workspacesByDomain.value(domain).size());
        result.append(domainObj);
    }
    return result;
}

QJsonObject RosInspector::inspectDomain(
    const QString& domainId,
    const QJsonArray& processes,
    bool includeGraphDetails) const {
    QJsonObject out;
    out.insert("domain_id", domainId);

    if (!isRos2Available()) {
        out.insert("error", "ros2 CLI is not available in PATH.");
        out.insert("nodes", QJsonArray{});
        out.insert("topic_qos", QJsonObject{});
        return out;
    }

    const QMap<QString, QString> env = rosEnv(domainId);
    const CommandResult nodeListResult =
        CommandRunner::run("ros2", {"node", "list"}, 5000, env);
    if (!nodeListResult.success()) {
        out.insert("error", "Failed to query ROS nodes.");
        out.insert("details", nodeListResult.stderrText);
        out.insert("nodes", QJsonArray{});
        out.insert("topic_qos", QJsonObject{});
        return out;
    }

    const QStringList nodeNames = parseLines(nodeListResult.stdoutText);
    QJsonArray nodes;
    QSet<QString> uniqueTopics;

    for (const QString& fullNodeName : nodeNames) {
        QJsonObject node;
        node.insert("domain_id", domainId);
        node.insert("full_name", fullNodeName);
        node.insert("node_name", baseNodeName(fullNodeName));
        node.insert("namespace", nodeNamespace(fullNodeName));

        const QJsonObject proc = findProcessForNode(fullNodeName, processes);
        if (!proc.isEmpty()) {
            node.insert("pid", proc.value("pid").toInt(-1));
            node.insert("executable", proc.value("executable").toString());
            node.insert("package", proc.value("package").toString());
            node.insert("workspace_origin", proc.value("workspace_origin").toString());
            node.insert("launch_source", proc.value("launch_source").toString());
            node.insert("cpu_percent", proc.value("cpu_percent").toDouble());
            node.insert("memory_percent", proc.value("memory_percent").toDouble());
            node.insert("threads", proc.value("threads").toInt());
        } else {
            node.insert("pid", -1);
            node.insert("executable", "");
            node.insert("package", "");
            node.insert("workspace_origin", "");
            node.insert("launch_source", "");
            node.insert("cpu_percent", 0.0);
            node.insert("memory_percent", 0.0);
            node.insert("threads", 0);
        }

        QJsonArray publishers;
        QJsonArray subscribers;
        QJsonArray serviceServers;
        QJsonArray serviceClients;
        QJsonArray actionServers;
        QJsonArray actionClients;

        if (includeGraphDetails) {
            const CommandResult nodeInfoResult = CommandRunner::run(
                "ros2", {"node", "info", fullNodeName}, 5000, env);
            if (nodeInfoResult.success()) {
                const QJsonObject nodeInfo = parseNodeInfoText(nodeInfoResult.stdoutText);
                publishers = nodeInfo.value("publishers").toArray();
                subscribers = nodeInfo.value("subscribers").toArray();
                serviceServers = nodeInfo.value("service_servers").toArray();
                serviceClients = nodeInfo.value("service_clients").toArray();
                actionServers = nodeInfo.value("action_servers").toArray();
                actionClients = nodeInfo.value("action_clients").toArray();
            }
        }

        node.insert("publishers", publishers);
        node.insert("subscribers", subscribers);
        node.insert("service_servers", serviceServers);
        node.insert("service_clients", serviceClients);
        node.insert("action_servers", actionServers);
        node.insert("action_clients", actionClients);

        for (const QJsonValue& pub : publishers) {
            const QString topic = pub.toObject().value("name").toString();
            if (!topic.isEmpty()) {
                uniqueTopics.insert(topic);
            }
        }
        for (const QJsonValue& sub : subscribers) {
            const QString topic = sub.toObject().value("name").toString();
            if (!topic.isEmpty()) {
                uniqueTopics.insert(topic);
            }
        }

        const CommandResult lifecycleGet =
            CommandRunner::run("ros2", {"lifecycle", "get", fullNodeName}, 2200, env);
        const bool lifecycleCapable = lifecycleGet.success();
        node.insert("lifecycle_capable", lifecycleCapable);
        node.insert(
            "lifecycle_state",
            lifecycleCapable ? parseLifecycleStateText(lifecycleGet.stdoutText) : QString("unsupported"));

        QJsonArray parameterNames;
        QJsonArray pluginHints;
        bool parametersSupported = false;
        if (includeGraphDetails) {
            const CommandResult paramList =
                CommandRunner::run("ros2", {"param", "list", fullNodeName}, 3500, env);
            parametersSupported = paramList.success();
            if (paramList.success()) {
                QSet<QString> uniqueParameters;
                for (const QString& raw : parseLines(paramList.stdoutText)) {
                    QString line = cleanGraphEntryLine(raw);
                    if (line.endsWith(':')) {
                        continue;
                    }
                    if (line == fullNodeName || line == (fullNodeName + ":")) {
                        continue;
                    }
                    if (!line.isEmpty()) {
                        uniqueParameters.insert(line);
                    }
                }
                QStringList ordered = uniqueParameters.values();
                std::sort(ordered.begin(), ordered.end());
                for (const QString& parameter : ordered) {
                    parameterNames.append(parameter);
                }

                int fetchedHints = 0;
                for (const QString& parameter : ordered) {
                    if (!isPluginLikeParameter(parameter)) {
                        continue;
                    }
                    QJsonObject hint;
                    hint.insert("parameter", parameter);
                    const CommandResult valueResult = CommandRunner::run(
                        "ros2", {"param", "get", fullNodeName, parameter}, 2000, env);
                    hint.insert(
                        "value",
                        valueResult.success() ? valueResult.stdoutText.trimmed() : QString("unavailable"));
                    pluginHints.append(hint);
                    fetchedHints++;
                    if (fetchedHints >= 6) {
                        break;
                    }
                }
            }
        }
        node.insert("parameters_supported", parametersSupported);
        node.insert("parameter_names", parameterNames);
        node.insert("parameter_count", parameterNames.size());
        node.insert("plugin_hints", pluginHints);

        QString runtimeClass = "idle";
        const double cpu = node.value("cpu_percent").toDouble();
        const int threads = node.value("threads").toInt();
        if (cpu >= 70.0) {
            runtimeClass = "cpu_bound";
        } else if (threads >= 40 && cpu < 50.0) {
            runtimeClass = "io_bound";
        } else if (publishers.size() >= 6) {
            runtimeClass = "network_heavy";
        } else if (cpu >= 15.0) {
            runtimeClass = "active";
        }
        node.insert("runtime_classification", runtimeClass);

        const QStringList behaviorRoles = inferBehaviorRoles(node);
        node.insert("behavior_roles", QJsonArray::fromStringList(behaviorRoles));
        node.insert("primary_behavior_role", behaviorRoles.isEmpty() ? "generic" : behaviorRoles.first());

        nodes.append(node);
    }

    QJsonObject topicQos;
    if (includeGraphDetails) {
        QStringList topics = uniqueTopics.values();
        std::sort(topics.begin(), topics.end());
        for (const QString& topic : topics) {
            if (topic.isEmpty()) {
                continue;
            }
            const CommandResult topicInfo =
                CommandRunner::run("ros2", {"topic", "info", "-v", topic}, 4000, env);
            if (topicInfo.success()) {
                topicQos.insert(topic, parseTopicInfoVerbose(topicInfo.stdoutText));
            }
        }
    }

    out.insert("nodes", nodes);
    out.insert("topic_qos", topicQos);
    return out;
}
QJsonObject RosInspector::inspectGraph(
    const QString& domainId,
    const QJsonArray& processes) const {
    QJsonObject domain = inspectDomain(domainId, processes, true);
    QJsonArray nodes = domain.value("nodes").toArray();

    QHash<QString, QSet<QString>> publishersByTopic;
    QHash<QString, QSet<QString>> subscribersByTopic;
    QHash<QString, QSet<QString>> serviceServersByName;
    QHash<QString, QSet<QString>> serviceClientsByName;
    QHash<QString, QSet<QString>> actionServersByName;
    QHash<QString, QSet<QString>> actionClientsByName;
    QHash<QString, QSet<QString>> topicAdjacency;
    QHash<QString, int> nodeNameCount;
    QHash<QString, int> roleCounts;
    QSet<QString> graphNodesFull;
    QSet<QString> graphNodesBase;

    QJsonArray isolatedNodes;
    QJsonObject nodeToPid;

    for (const QJsonValue& nodeValue : nodes) {
        const QJsonObject node = nodeValue.toObject();
        const QString fullName = node.value("full_name").toString();
        const QString baseName = node.value("node_name").toString();
        nodeNameCount[fullName] += 1;
        graphNodesFull.insert(fullName);
        graphNodesBase.insert(baseName);
        nodeToPid.insert(fullName, node.value("pid").toInt(-1));

        const QJsonArray roles = node.value("behavior_roles").toArray();
        for (const QJsonValue& roleValue : roles) {
            const QString role = roleValue.toString();
            if (!role.isEmpty()) {
                roleCounts[role] += 1;
            }
        }

        const QJsonArray publishers = node.value("publishers").toArray();
        const QJsonArray subscribers = node.value("subscribers").toArray();
        const QJsonArray serviceServers = node.value("service_servers").toArray();
        const QJsonArray serviceClients = node.value("service_clients").toArray();
        const QJsonArray actionServers = node.value("action_servers").toArray();
        const QJsonArray actionClients = node.value("action_clients").toArray();

        if (publishers.isEmpty() && subscribers.isEmpty()
            && serviceServers.isEmpty() && serviceClients.isEmpty()
            && actionServers.isEmpty() && actionClients.isEmpty()) {
            isolatedNodes.append(fullName);
        }

        for (const QJsonValue& pubValue : publishers) {
            const QString topic = pubValue.toObject().value("name").toString();
            if (!topic.isEmpty()) {
                publishersByTopic[topic].insert(fullName);
            }
        }
        for (const QJsonValue& subValue : subscribers) {
            const QString topic = subValue.toObject().value("name").toString();
            if (!topic.isEmpty()) {
                subscribersByTopic[topic].insert(fullName);
            }
        }
        for (const QJsonValue& srv : serviceServers) {
            const QString service = srv.toObject().value("name").toString();
            if (!service.isEmpty()) {
                serviceServersByName[service].insert(fullName);
            }
        }
        for (const QJsonValue& cli : serviceClients) {
            const QString service = cli.toObject().value("name").toString();
            if (!service.isEmpty()) {
                serviceClientsByName[service].insert(fullName);
            }
        }
        for (const QJsonValue& srv : actionServers) {
            const QString action = srv.toObject().value("name").toString();
            if (!action.isEmpty()) {
                actionServersByName[action].insert(fullName);
            }
        }
        for (const QJsonValue& cli : actionClients) {
            const QString action = cli.toObject().value("name").toString();
            if (!action.isEmpty()) {
                actionClientsByName[action].insert(fullName);
            }
        }
    }

    QJsonArray topics;
    QJsonArray noSubscriberTopics;
    QJsonArray noPublisherTopics;
    QJsonArray tfWarnings;
    QSet<QString> allTopics;
    for (auto it = publishersByTopic.constBegin(); it != publishersByTopic.constEnd(); ++it) {
        allTopics.insert(it.key());
    }
    for (auto it = subscribersByTopic.constBegin(); it != subscribersByTopic.constEnd(); ++it) {
        allTopics.insert(it.key());
    }

    QStringList sortedTopics = allTopics.values();
    std::sort(sortedTopics.begin(), sortedTopics.end());
    for (const QString& topic : sortedTopics) {
        const QSet<QString> pubs = publishersByTopic.value(topic);
        const QSet<QString> subs = subscribersByTopic.value(topic);
        QJsonObject topicObj;
        topicObj.insert("topic", topic);
        topicObj.insert("publishers", toJsonArray(pubs));
        topicObj.insert("subscribers", toJsonArray(subs));
        topicObj.insert("publisher_count", pubs.size());
        topicObj.insert("subscriber_count", subs.size());
        topics.append(topicObj);

        if (!pubs.isEmpty() && subs.isEmpty()) {
            noSubscriberTopics.append(topic);
        }
        if (pubs.isEmpty() && !subs.isEmpty()) {
            noPublisherTopics.append(topic);
        }

        if ((topic == "/tf" || topic == "/tf_static") && pubs.size() > 1) {
            tfWarnings.append(QString("Multiple publishers detected on %1").arg(topic));
        }

        for (const QString& publisher : pubs.values()) {
            for (const QString& subscriber : subs.values()) {
                if (publisher != subscriber) {
                    topicAdjacency[publisher].insert(subscriber);
                }
            }
        }
    }

    QJsonArray duplicates;
    for (auto it = nodeNameCount.constBegin(); it != nodeNameCount.constEnd(); ++it) {
        if (it.value() > 1) {
            QJsonObject dup;
            dup.insert("node", it.key());
            dup.insert("count", it.value());
            duplicates.append(dup);
        }
    }

    QJsonArray serviceEdges;
    QJsonArray missingServiceServers;
    QSet<QString> allServices;
    for (auto it = serviceServersByName.constBegin(); it != serviceServersByName.constEnd(); ++it) {
        allServices.insert(it.key());
    }
    for (auto it = serviceClientsByName.constBegin(); it != serviceClientsByName.constEnd(); ++it) {
        allServices.insert(it.key());
    }
    for (const QString& service : allServices.values()) {
        const QSet<QString> servers = serviceServersByName.value(service);
        const QSet<QString> clients = serviceClientsByName.value(service);
        if (servers.isEmpty() && !clients.isEmpty()) {
            QJsonObject row;
            row.insert("service", service);
            row.insert("clients", toJsonArray(clients));
            missingServiceServers.append(row);
        }
        for (const QString& client : clients.values()) {
            for (const QString& server : servers.values()) {
                QJsonObject edge;
                edge.insert("service", service);
                edge.insert("client_node", client);
                edge.insert("server_node", server);
                serviceEdges.append(edge);
            }
        }
    }

    QJsonArray actionEdges;
    QJsonArray missingActionServers;
    QSet<QString> allActions;
    for (auto it = actionServersByName.constBegin(); it != actionServersByName.constEnd(); ++it) {
        allActions.insert(it.key());
    }
    for (auto it = actionClientsByName.constBegin(); it != actionClientsByName.constEnd(); ++it) {
        allActions.insert(it.key());
    }
    for (const QString& action : allActions.values()) {
        const QSet<QString> servers = actionServersByName.value(action);
        const QSet<QString> clients = actionClientsByName.value(action);
        if (servers.isEmpty() && !clients.isEmpty()) {
            QJsonObject row;
            row.insert("action", action);
            row.insert("clients", toJsonArray(clients));
            missingActionServers.append(row);
        }
        for (const QString& client : clients.values()) {
            for (const QString& server : servers.values()) {
                QJsonObject edge;
                edge.insert("action", action);
                edge.insert("client_node", client);
                edge.insert("server_node", server);
                actionEdges.append(edge);
            }
        }
    }
    QSet<QString> cycleStrings;
    QHash<QString, int> visitState;
    QStringList stack;
    std::function<void(const QString&)> dfs = [&](const QString& node) {
        visitState[node] = 1;
        stack.append(node);
        for (const QString& child : topicAdjacency.value(node).values()) {
            if (visitState.value(child, 0) == 0) {
                dfs(child);
            } else if (visitState.value(child, 0) == 1) {
                const int startIdx = stack.lastIndexOf(child);
                if (startIdx >= 0) {
                    QStringList cycle = stack.mid(startIdx);
                    cycle.append(child);
                    cycleStrings.insert(cycle.join(" -> "));
                }
            }
        }
        stack.removeLast();
        visitState[node] = 2;
    };
    for (const QString& node : graphNodesFull.values()) {
        if (visitState.value(node, 0) == 0) {
            dfs(node);
        }
    }
    QStringList cycleList = cycleStrings.values();
    std::sort(cycleList.begin(), cycleList.end());
    QJsonArray circularDependencies = QJsonArray::fromStringList(cycleList);

    QVector<QJsonObject> criticalNodes;
    for (const QString& node : graphNodesFull.values()) {
        QSet<QString> visited;
        QList<QString> queue = {node};
        while (!queue.isEmpty()) {
            const QString current = queue.takeFirst();
            for (const QString& child : topicAdjacency.value(current).values()) {
                if (!visited.contains(child)) {
                    visited.insert(child);
                    queue.append(child);
                }
            }
        }
        if (visited.size() >= 3) {
            criticalNodes.push_back(QJsonObject{
                {"node", node},
                {"downstream_count", visited.size()},
            });
        }
    }
    std::sort(criticalNodes.begin(), criticalNodes.end(), [](const QJsonObject& a, const QJsonObject& b) {
        return a.value("downstream_count").toInt() > b.value("downstream_count").toInt();
    });
    QJsonArray singlePointsOfFailure;
    const int topCount = std::min(10, static_cast<int>(criticalNodes.size()));
    for (int i = 0; i < topCount; ++i) {
        singlePointsOfFailure.append(criticalNodes[static_cast<qsizetype>(i)]);
    }

    QJsonArray misinitializedProcesses;
    for (const QJsonValue& procValue : processes) {
        const QJsonObject proc = procValue.toObject();
        if (!proc.value("is_ros").toBool()) {
            continue;
        }
        if (proc.value("ros_domain_id").toString("0") != domainId) {
            continue;
        }
        const QString procNode = proc.value("node_name").toString();
        if (procNode.isEmpty()) {
            continue;
        }
        if (!graphNodesBase.contains(procNode)) {
            QJsonObject row;
            row.insert("pid", proc.value("pid").toInt(-1));
            row.insert("node_name", procNode);
            row.insert("executable", proc.value("executable").toString());
            row.insert("workspace_origin", proc.value("workspace_origin").toString());
            misinitializedProcesses.append(row);
        }
    }

    QJsonObject roleSummary;
    for (auto it = roleCounts.constBegin(); it != roleCounts.constEnd(); ++it) {
        roleSummary.insert(it.key(), it.value());
    }

    QJsonObject graph;
    graph.insert("domain_id", domainId);
    graph.insert("nodes", nodes);
    graph.insert("node_to_pid", nodeToPid);
    graph.insert("topics", topics);
    graph.insert("topic_qos", domain.value("topic_qos"));
    graph.insert("publishers_without_subscribers", noSubscriberTopics);
    graph.insert("subscribers_without_publishers", noPublisherTopics);
    graph.insert("missing_service_servers", missingServiceServers);
    graph.insert("missing_action_servers", missingActionServers);
    graph.insert("service_edges", serviceEdges);
    graph.insert("action_edges", actionEdges);
    graph.insert("isolated_nodes", isolatedNodes);
    graph.insert("circular_dependencies", circularDependencies);
    graph.insert("single_points_of_failure", singlePointsOfFailure);
    graph.insert("duplicate_node_names", duplicates);
    graph.insert("misinitialized_processes", misinitializedProcesses);
    graph.insert("tf_warnings", tfWarnings);
    graph.insert("role_summary", roleSummary);
    return graph;
}

QJsonObject RosInspector::inspectTfNav2(const QString& domainId) const {
    QJsonObject out;
    out.insert("domain_id", domainId);

    if (!isRos2Available()) {
        out.insert("error", "ros2 CLI is not available in PATH.");
        out.insert("tf_edges", QJsonArray{});
        out.insert("tf_warnings", QJsonArray{});
        out.insert("runtime", QJsonObject{});
        out.insert("nav2", QJsonObject{});
        return out;
    }

    const QMap<QString, QString> env = rosEnv(domainId);
    const CommandResult topicsWithTypes =
        CommandRunner::run("ros2", {"topic", "list", "-t"}, 4500, env);

    QSet<QString> tfTopics;
    QSet<QString> actionStatusTopics;
    if (topicsWithTypes.success()) {
        for (const QJsonValue& value : parseTopicListWithTypes(topicsWithTypes.stdoutText)) {
            const QJsonObject row = value.toObject();
            const QString topic = row.value("topic").toString();
            const QString type = row.value("type").toString();
            if (topic.isEmpty()) {
                continue;
            }

            if (type == "tf2_msgs/msg/TFMessage"
                || topic == "/tf" || topic == "/tf_static"
                || topic.endsWith("/tf") || topic.endsWith("/tf_static")) {
                tfTopics.insert(topic);
            }

            if (type == "action_msgs/msg/GoalStatusArray" && topic.contains("_action/status")) {
                actionStatusTopics.insert(topic);
            }
        }
    }
    out.insert("tf_topics", toJsonArray(tfTopics));

    QJsonArray tfEdges;
    QSet<QString> edgeKeys;
    QJsonArray tfWarnings;

    QStringList orderedTfTopics = tfTopics.values();
    std::sort(orderedTfTopics.begin(), orderedTfTopics.end());
    const int maxTfTopics = std::min(6, static_cast<int>(orderedTfTopics.size()));
    for (int i = 0; i < maxTfTopics; ++i) {
        const QString topic = orderedTfTopics.at(i);
        const CommandResult tfEcho =
            CommandRunner::run("ros2", {"topic", "echo", topic, "--once"}, 2600, env);
        if (tfEcho.success()) {
            for (const QJsonValue& edgeValue : parseTfEdges(tfEcho.stdoutText)) {
                const QJsonObject edge = edgeValue.toObject();
                const QString key =
                    edge.value("parent").toString() + "->" + edge.value("child").toString();
                if (!edgeKeys.contains(key)) {
                    edgeKeys.insert(key);
                    QJsonObject row = edge;
                    row.insert("topic", topic);
                    tfEdges.append(row);
                }
            }
        }

        const CommandResult topicInfo =
            CommandRunner::run("ros2", {"topic", "info", "-v", topic}, 2800, env);
        if (topicInfo.success()) {
            int publishers = 0;
            for (const QString& line : topicInfo.stdoutText.split('\n', Qt::SkipEmptyParts)) {
                if (line.trimmed().startsWith("Node name:")) {
                    publishers++;
                }
            }
            if (publishers > 1) {
                tfWarnings.append(QString("Multiple publishers detected on %1").arg(topic));
            }
        }
    }
    out.insert("tf_edges", tfEdges);

    QHash<QString, int> childCount;
    for (const QJsonValue& value : tfEdges) {
        const QString child = value.toObject().value("child").toString();
        if (!child.isEmpty()) {
            childCount[child] += 1;
        }
    }
    for (auto it = childCount.constBegin(); it != childCount.constEnd(); ++it) {
        if (it.value() > 1) {
            tfWarnings.append(
                QString("Frame '%1' appears with multiple parents/publishers.").arg(it.key()));
        }
    }
    out.insert("tf_warnings", tfWarnings);

    QJsonObject runtime;
    QJsonArray lifecycleStates;
    const CommandResult lifecycleNodes =
        CommandRunner::run("ros2", {"lifecycle", "nodes"}, 3500, env);
    if (lifecycleNodes.success()) {
        const QStringList lifecycleNodeNames = parseLines(lifecycleNodes.stdoutText);
        for (const QString& node : lifecycleNodeNames) {
            if (!node.startsWith('/')) {
                continue;
            }
            const CommandResult state =
                CommandRunner::run("ros2", {"lifecycle", "get", node}, 2600, env);
            QJsonObject lifecycle;
            lifecycle.insert("node", node);
            lifecycle.insert("state", state.success() ? parseLifecycleStateText(state.stdoutText) : "unknown");
            lifecycleStates.append(lifecycle);
        }
    }
    runtime.insert("lifecycle_states", lifecycleStates);

    QJsonArray actionStatus;
    QJsonArray activeActionTopics;
    bool goalActive = false;
    QStringList orderedActionTopics = actionStatusTopics.values();
    std::sort(orderedActionTopics.begin(), orderedActionTopics.end());
    const int maxActionTopics = std::min(10, static_cast<int>(orderedActionTopics.size()));
    for (int i = 0; i < maxActionTopics; ++i) {
        const QString topic = orderedActionTopics.at(i);
        const CommandResult status =
            CommandRunner::run("ros2", {"topic", "echo", topic, "--once"}, 2400, env);
        bool active = false;
        if (status.success()) {
            active = !status.stdoutText.contains("status_list: []");
        }
        if (active) {
            goalActive = true;
            activeActionTopics.append(topic);
        }
        QJsonObject row;
        row.insert("topic", topic);
        row.insert("active", active);
        row.insert(
            "sample",
            status.success() ? status.stdoutText.left(280).trimmed() : status.stderrText.left(280).trimmed());
        actionStatus.append(row);
    }
    runtime.insert("action_status", actionStatus);
    runtime.insert("active_action_topics", activeActionTopics);
    runtime.insert("goal_active", goalActive);

    out.insert("runtime", runtime);
    out.insert("nav2", runtime);
    return out;
}

QJsonObject RosInspector::fetchNodeParameters(
    const QString& domainId,
    const QString& nodeName) const {
    QJsonObject out;
    out.insert("domain_id", domainId);
    out.insert("node", nodeName);

    if (!isRos2Available()) {
        out.insert("success", false);
        out.insert("parameters", "");
        out.insert("error", "ros2 CLI is not available in PATH.");
        return out;
    }

    const CommandResult result =
        CommandRunner::run("ros2", {"param", "dump", nodeName}, 6000, rosEnv(domainId));
    out.insert("success", result.success());
    out.insert("parameters", result.stdoutText);
    out.insert("error", result.stderrText);
    return out;
}

}  // namespace rrcc
