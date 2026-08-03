#include "ShellCommand.h"

namespace fc {

ShellInvocation shellInvocationFor(const QString &command) {
#ifdef Q_OS_WIN
    // /c: run this and exit. cmd.exe takes the rest of the line verbatim, so a
    // single argument is right here -- QProcess must not requote it.
    return {QStringLiteral("cmd.exe"), {QStringLiteral("/c"), command}};
#else
    return {QStringLiteral("/bin/sh"), {QStringLiteral("-c"), command}};
#endif
}

} // namespace fc
