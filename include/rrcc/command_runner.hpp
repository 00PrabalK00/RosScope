#pragma once

#include <QMap>
#include <QString>
#include <QStringList>

namespace rrcc {

struct CommandResult {
    int exitCode = -1;
    QString stdoutText;
    QString stderrText;
    bool timedOut = false;

    [[nodiscard]] bool success() const { return !timedOut && exitCode == 0; }
};

class CommandRunner {
public:
    static CommandResult run(
        const QString& program,
        const QStringList& args = {},
        int timeoutMs = 3000,
        const QMap<QString, QString>& extraEnv = {});

    static CommandResult runShell(
        const QString& command,
        int timeoutMs = 3000,
        const QMap<QString, QString>& extraEnv = {});
};

}  // namespace rrcc

