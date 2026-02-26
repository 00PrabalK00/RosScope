#include "rrcc/command_runner.hpp"

#include <QElapsedTimer>
#include <QProcess>
#include <QProcessEnvironment>

#include "rrcc/telemetry.hpp"

namespace rrcc {

CommandResult CommandRunner::run(
    const QString& program,
    const QStringList& args,
    int timeoutMs,
    const QMap<QString, QString>& extraEnv) {
    QProcess process;
    QElapsedTimer elapsed;
    elapsed.start();
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (auto it = extraEnv.constBegin(); it != extraEnv.constEnd(); ++it) {
        env.insert(it.key(), it.value());
    }

    process.setProcessEnvironment(env);
    process.start(program, args);

    CommandResult result;
    if (!process.waitForStarted(timeoutMs)) {
        result.timedOut = true;
        result.stderrText = "Failed to start process.";
        Telemetry::instance().incrementCounter("commands.start_failures");
        Telemetry::instance().recordDurationMs("commands.duration_ms", elapsed.elapsed());
        return result;
    }

    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(500);
        result.timedOut = true;
        result.stderrText = "Command timed out.";
        Telemetry::instance().incrementCounter("commands.timeouts");
        Telemetry::instance().recordDurationMs("commands.duration_ms", elapsed.elapsed());
        return result;
    }

    result.exitCode = process.exitCode();
    result.stdoutText = QString::fromUtf8(process.readAllStandardOutput());
    result.stderrText = QString::fromUtf8(process.readAllStandardError());
    Telemetry::instance().incrementCounter("commands.count");
    if (result.exitCode != 0) {
        Telemetry::instance().incrementCounter("commands.non_zero_exit");
    }
    Telemetry::instance().recordDurationMs("commands.duration_ms", elapsed.elapsed());
    return result;
}

CommandResult CommandRunner::runShell(
    const QString& command,
    int timeoutMs,
    const QMap<QString, QString>& extraEnv) {
    return run("/bin/bash", {"-lc", command}, timeoutMs, extraEnv);
}

}  // namespace rrcc
