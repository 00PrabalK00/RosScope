#include "rrcc/system_monitor.hpp"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QNetworkAddressEntry>
#include <QNetworkInterface>
#include <QStringList>

#ifdef __linux__
#include <sys/statvfs.h>
#endif

#include "rrcc/command_runner.hpp"

namespace {

QString readTextFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

QMap<QString, qulonglong> parseMemInfo() {
    QMap<QString, qulonglong> values;
    const QString memInfo = readTextFile("/proc/meminfo");
    for (const QString& line : memInfo.split('\n', Qt::SkipEmptyParts)) {
        const int idx = line.indexOf(':');
        if (idx <= 0) {
            continue;
        }
        const QString key = line.left(idx).trimmed();
        const QString value = line.mid(idx + 1).trimmed().split(' ').value(0);
        values.insert(key, value.toULongLong());
    }
    return values;
}

QPair<qulonglong, qulonglong> parseCpuTimes() {
    const QString stat = readTextFile("/proc/stat");
    const QString first = stat.split('\n').value(0);
    const QStringList fields = first.split(' ', Qt::SkipEmptyParts);
    if (fields.size() < 8) {
        return {0, 0};
    }
    qulonglong total = 0;
    for (int i = 1; i < fields.size(); ++i) {
        total += fields[i].toULongLong();
    }
    const qulonglong idle = fields[4].toULongLong() + fields[5].toULongLong();
    return {total, idle};
}

}  // namespace

namespace rrcc {

QJsonObject SystemMonitor::cpuSnapshot() {
    QJsonObject cpu;
    const auto [total, idle] = parseCpuTimes();
    cpu.insert("total_jiffies", static_cast<qint64>(total));
    cpu.insert("idle_jiffies", static_cast<qint64>(idle));
    return cpu;
}

QJsonObject SystemMonitor::memorySnapshot() {
    QJsonObject mem;
    const QMap<QString, qulonglong> info = parseMemInfo();
    const qulonglong total = info.value("MemTotal");
    const qulonglong available = info.value("MemAvailable");
    const qulonglong used = (total > available) ? (total - available) : 0;

    mem.insert("total_kb", static_cast<qint64>(total));
    mem.insert("available_kb", static_cast<qint64>(available));
    mem.insert("used_kb", static_cast<qint64>(used));
    mem.insert("used_percent", total == 0 ? 0.0 : (100.0 * static_cast<double>(used) / total));
    return mem;
}

QJsonObject SystemMonitor::diskSnapshot() {
    QJsonObject disk;
#ifdef __linux__
    struct statvfs fs {};
    if (statvfs("/", &fs) == 0) {
        const qulonglong total = static_cast<qulonglong>(fs.f_blocks) * fs.f_frsize;
        const qulonglong free = static_cast<qulonglong>(fs.f_bavail) * fs.f_frsize;
        const qulonglong used = (total > free) ? (total - free) : 0;
        disk.insert("total_bytes", static_cast<qint64>(total));
        disk.insert("free_bytes", static_cast<qint64>(free));
        disk.insert("used_bytes", static_cast<qint64>(used));
        disk.insert("used_percent", total == 0 ? 0.0 : (100.0 * static_cast<double>(used) / total));
    }
#endif
    return disk;
}

QJsonArray SystemMonitor::gpuSnapshot() {
    QJsonArray gpus;
    const CommandResult result = CommandRunner::run(
        "nvidia-smi",
        {"--query-gpu=name,utilization.gpu,memory.used,memory.total", "--format=csv,noheader,nounits"},
        2500);
    if (!result.success()) {
        return gpus;
    }

    for (const QString& line : result.stdoutText.split('\n', Qt::SkipEmptyParts)) {
        const QStringList parts = line.split(',', Qt::SkipEmptyParts);
        if (parts.size() < 4) {
            continue;
        }
        QJsonObject gpu;
        gpu.insert("name", parts[0].trimmed());
        gpu.insert("utilization_percent", parts[1].trimmed().toDouble());
        gpu.insert("memory_used_mb", parts[2].trimmed().toDouble());
        gpu.insert("memory_total_mb", parts[3].trimmed().toDouble());
        gpus.append(gpu);
    }
    return gpus;
}

QJsonArray SystemMonitor::usbDevices() {
    QJsonArray devices;
    const CommandResult result = CommandRunner::run("lsusb", {}, 2500);
    if (!result.success()) {
        return devices;
    }
    for (const QString& line : result.stdoutText.split('\n', Qt::SkipEmptyParts)) {
        devices.append(line.trimmed());
    }
    return devices;
}

QJsonArray SystemMonitor::serialPorts() {
    QJsonArray ports;
    const QDir devDir("/dev");
    const QStringList patterns = {"ttyUSB*", "ttyACM*", "ttyS*", "ttyAMA*"};
    const QStringList entries = devDir.entryList(patterns, QDir::System | QDir::NoDotAndDotDot);
    for (const QString& entry : entries) {
        ports.append("/dev/" + entry);
    }
    return ports;
}

QJsonArray SystemMonitor::canInterfaces() {
    QJsonArray can;
    const CommandResult result =
        CommandRunner::run("ip", {"-details", "-brief", "link", "show", "type", "can"}, 2500);
    if (!result.success()) {
        return can;
    }
    for (const QString& line : result.stdoutText.split('\n', Qt::SkipEmptyParts)) {
        can.append(line.trimmed());
    }
    return can;
}

QJsonArray SystemMonitor::networkInterfaces() {
    QJsonArray interfaces;
    const QList<QNetworkInterface> all = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface& iface : all) {
        QJsonObject obj;
        obj.insert("name", iface.name());
        obj.insert("is_up", iface.flags().testFlag(QNetworkInterface::IsUp));
        obj.insert("is_running", iface.flags().testFlag(QNetworkInterface::IsRunning));

        QJsonArray addresses;
        const QList<QNetworkAddressEntry> entries = iface.addressEntries();
        for (const QNetworkAddressEntry& entry : entries) {
            addresses.append(entry.ip().toString());
        }
        obj.insert("addresses", addresses);

        const QString base = "/sys/class/net/" + iface.name() + "/statistics/";
        const QString rxText = readTextFile(base + "rx_bytes").trimmed();
        const QString txText = readTextFile(base + "tx_bytes").trimmed();
        obj.insert("rx_bytes", static_cast<qint64>(rxText.toLongLong()));
        obj.insert("tx_bytes", static_cast<qint64>(txText.toLongLong()));

        interfaces.append(obj);
    }
    return interfaces;
}

QJsonObject SystemMonitor::collectSystem() {
    QJsonObject out;

    const auto [currentTotal, currentIdle] = parseCpuTimes();
    double cpuPercent = 0.0;
    if (!firstCpuSample_ && currentTotal > previousCpuTotal_) {
        const qulonglong deltaTotal = currentTotal - previousCpuTotal_;
        const qulonglong deltaIdle = currentIdle - previousCpuIdle_;
        cpuPercent =
            100.0 * (1.0 - (static_cast<double>(deltaIdle) / static_cast<double>(deltaTotal)));
        if (cpuPercent < 0.0) {
            cpuPercent = 0.0;
        }
    }
    previousCpuTotal_ = currentTotal;
    previousCpuIdle_ = currentIdle;
    firstCpuSample_ = false;

    QJsonObject cpu;
    cpu.insert("usage_percent", cpuPercent);
    out.insert("cpu", cpu);
    out.insert("memory", memorySnapshot());
    out.insert("disk", diskSnapshot());
    out.insert("gpus", gpuSnapshot());
    out.insert("usb_devices", usbDevices());
    out.insert("serial_ports", serialPorts());
    out.insert("can_interfaces", canInterfaces());
    out.insert("network_interfaces", networkInterfaces());
    return out;
}

QString SystemMonitor::tailDmesg(int lines) const {
    const QString cmd = QString("dmesg --ctime --color=never | tail -n %1").arg(lines);
    const CommandResult result = CommandRunner::runShell(cmd, 4000);
    if (result.success()) {
        return result.stdoutText;
    }
    return result.stderrText.isEmpty() ? QString("dmesg is unavailable.") : result.stderrText;
}

}  // namespace rrcc
