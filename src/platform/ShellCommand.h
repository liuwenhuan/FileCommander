#pragma once

#include <QString>
#include <QStringList>

namespace fc {

// How to hand a typed command line to this platform's shell.
struct ShellInvocation {
    QString program;
    QStringList arguments;
};

// Wraps `command` for the platform's command interpreter, so what the user
// typed behaves the way it does in a terminal: pipes, redirection, quoting and
// -- the reason this exists at all -- built-in commands. `dir`, `echo` and
// `copy` on Windows are not executables, they only exist inside cmd.exe, so
// starting them as processes fails with nothing but "failed to start".
ShellInvocation shellInvocationFor(const QString &command);

} // namespace fc
