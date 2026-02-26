#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>

namespace rrcc {

class SystemMonitor {
public:
    SystemMonitor() = default;

    QJsonObject collectSystem();
    QString tailDmesg(int lines) const;

private:
    static QJsonObject cpuSnapshot();
    static QJsonObject memorySnapshot();
    static QJsonObject diskSnapshot();
    static QJsonArray gpuSnapshot();
    static QJsonArray usbDevices();
    static QJsonArray serialPorts();
    static QJsonArray canInterfaces();
    static QJsonArray networkInterfaces();

    qulonglong previousCpuTotal_ = 0;
    qulonglong previousCpuIdle_ = 0;
    bool firstCpuSample_ = true;
};

}  // namespace rrcc

