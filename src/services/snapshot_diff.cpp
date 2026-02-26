#include "rrcc/snapshot_diff.hpp"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QStringList>

#include <algorithm>

namespace rrcc {

namespace {

QSet<QString> toSet(const QJsonArray& array) {
    QSet<QString> out;
    for (const QJsonValue& value : array) {
        out.insert(value.toString());
    }
    return out;
}

QJsonArray sortedArray(const QSet<QString>& set) {
    QStringList list = set.values();
    std::sort(list.begin(), list.end());
    return QJsonArray::fromStringList(list);
}

QString sha(const QString& value) {
    return QString::fromLatin1(
        QCryptographicHash::hash(value.toUtf8(), QCryptographicHash::Sha256).toHex());
}

}  // namespace

QJsonArray SnapshotDiff::nodeList(const QJsonObject& snapshot) {
    QJsonArray out;
    for (const QJsonValue& nodeValue : snapshot.value("graph").toObject().value("nodes").toArray()) {
        out.append(nodeValue.toObject().value("full_name").toString());
    }
    return out;
}

QJsonArray SnapshotDiff::topicList(const QJsonObject& snapshot) {
    QJsonArray out;
    for (const QJsonValue& topicValue : snapshot.value("graph").toObject().value("topics").toArray()) {
        out.append(topicValue.toObject().value("topic").toString());
    }
    return out;
}

QJsonArray SnapshotDiff::domainList(const QJsonObject& snapshot) {
    QJsonArray out;
    for (const QJsonValue& domainValue : snapshot.value("domains").toArray()) {
        out.append(domainValue.toObject().value("domain_id").toString("0"));
    }
    return out;
}

QJsonObject SnapshotDiff::paramHashes(const QJsonObject& snapshot) {
    QJsonObject hashes;
    const QJsonObject params = snapshot.value("parameters").toObject();
    for (auto it = params.constBegin(); it != params.constEnd(); ++it) {
        hashes.insert(it.key(), sha(it.value().toString()));
    }
    return hashes;
}

QJsonObject SnapshotDiff::compare(const QJsonObject& left, const QJsonObject& right) const {
    const QSet<QString> leftNodes = toSet(nodeList(left));
    const QSet<QString> rightNodes = toSet(nodeList(right));
    const QSet<QString> leftTopics = toSet(topicList(left));
    const QSet<QString> rightTopics = toSet(topicList(right));
    const QSet<QString> leftDomains = toSet(domainList(left));
    const QSet<QString> rightDomains = toSet(domainList(right));

    QSet<QString> nodesAdded = rightNodes - leftNodes;
    QSet<QString> nodesRemoved = leftNodes - rightNodes;
    QSet<QString> topicsAdded = rightTopics - leftTopics;
    QSet<QString> topicsRemoved = leftTopics - rightTopics;
    QSet<QString> domainsAdded = rightDomains - leftDomains;
    QSet<QString> domainsRemoved = leftDomains - rightDomains;

    const QJsonObject leftParamHashes = paramHashes(left);
    const QJsonObject rightParamHashes = paramHashes(right);
    QSet<QString> allParamNodes;
    for (const QString& key : leftParamHashes.keys()) {
        allParamNodes.insert(key);
    }
    for (const QString& key : rightParamHashes.keys()) {
        allParamNodes.insert(key);
    }

    QJsonArray paramChanged;
    for (const QString& node : allParamNodes.values()) {
        const QString l = leftParamHashes.value(node).toString();
        const QString r = rightParamHashes.value(node).toString();
        if (l != r) {
            paramChanged.append(node);
        }
    }

    QJsonObject summary;
    summary.insert("nodes_added", nodesAdded.size());
    summary.insert("nodes_removed", nodesRemoved.size());
    summary.insert("topics_added", topicsAdded.size());
    summary.insert("topics_removed", topicsRemoved.size());
    summary.insert("domains_added", domainsAdded.size());
    summary.insert("domains_removed", domainsRemoved.size());
    summary.insert("parameters_changed", paramChanged.size());

    QJsonObject diff;
    diff.insert("summary", summary);
    diff.insert("nodes_added", sortedArray(nodesAdded));
    diff.insert("nodes_removed", sortedArray(nodesRemoved));
    diff.insert("topics_added", sortedArray(topicsAdded));
    diff.insert("topics_removed", sortedArray(topicsRemoved));
    diff.insert("domains_added", sortedArray(domainsAdded));
    diff.insert("domains_removed", sortedArray(domainsRemoved));
    diff.insert("parameters_changed", paramChanged);
    return diff;
}

QJsonObject SnapshotDiff::compareFiles(const QString& leftPath, const QString& rightPath) const {
    QFile leftFile(leftPath);
    QFile rightFile(rightPath);
    if (!leftFile.open(QIODevice::ReadOnly)) {
        return {{"success", false}, {"error", "Failed to open left snapshot."}};
    }
    if (!rightFile.open(QIODevice::ReadOnly)) {
        return {{"success", false}, {"error", "Failed to open right snapshot."}};
    }

    const QJsonDocument leftDoc = QJsonDocument::fromJson(leftFile.readAll());
    const QJsonDocument rightDoc = QJsonDocument::fromJson(rightFile.readAll());
    if (!leftDoc.isObject() || !rightDoc.isObject()) {
        return {{"success", false}, {"error", "Snapshot files must be JSON objects."}};
    }

    QJsonObject out = compare(leftDoc.object(), rightDoc.object());
    out.insert("success", true);
    out.insert("left_path", leftPath);
    out.insert("right_path", rightPath);
    return out;
}

}  // namespace rrcc
