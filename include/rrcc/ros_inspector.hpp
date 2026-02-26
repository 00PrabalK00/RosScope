#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>

namespace rrcc {

class RosInspector {
public:
    RosInspector() = default;

    QJsonArray listDomains(const QJsonArray& processes) const;
    QJsonObject inspectDomain(
        const QString& domainId,
        const QJsonArray& processes,
        bool includeGraphDetails = false) const;
    QJsonObject inspectGraph(const QString& domainId, const QJsonArray& processes) const;
    QJsonObject inspectTfNav2(const QString& domainId) const;
    QJsonObject fetchNodeParameters(const QString& domainId, const QString& nodeName) const;

private:
    bool isRos2Available() const;

    static QMap<QString, QString> rosEnv(const QString& domainId);
    static QStringList parseLines(const QString& text);
    static QString baseNodeName(const QString& fullName);
    static QString nodeNamespace(const QString& fullName);
    static QJsonObject findProcessForNode(const QString& fullNodeName, const QJsonArray& processes);
    static QJsonObject parseNodeInfoText(const QString& nodeInfoText);
    static QJsonObject parseTopicInfoVerbose(const QString& topicInfoText);
    static QJsonArray parseTfEdges(const QString& tfEchoText);

    mutable bool ros2Checked_ = false;
    mutable bool ros2Available_ = false;
};

}  // namespace rrcc
