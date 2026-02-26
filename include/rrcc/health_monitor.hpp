#pragma once

#include <QJsonArray>
#include <QJsonObject>

namespace rrcc {

class HealthMonitor {
public:
    HealthMonitor() = default;

    QJsonObject evaluate(
        const QJsonArray& domains,
        const QJsonObject& graph,
        const QJsonObject& tfNav2) const;
};

}  // namespace rrcc

