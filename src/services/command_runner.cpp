#include "rrcc/command_runner.hpp"

#include <QProcess>
#include <QProcessEnvironment>

namespace rrcc {

CommandResult CommandRunner::run(
    const QString& program,
    const QStringList& args,
    int timeoutMs,
    const QMap<QString, QString>& extraEnv) {
    QProcess process;
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
        return result;
    }

    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(500);
        result.timedOut = true;
        result.stderrText = "Command timed out.";
        return result;
    }

    result.exitCode = process.exitCode();
    result.stdoutText = QString::fromUtf8(process.readAllStandardOutput());
    result.stderrText = QString::fromUtf8(process.readAllStandardError());
    return result;
}

CommandResult CommandRunner::runShell(
    const QString& command,
    int timeoutMs,
    const QMap<QString, QString>& extraEnv) {
    return run("/bin/bash", {"-lc", command}, timeoutMs, extraEnv);
}

}  // namespace rrcc

