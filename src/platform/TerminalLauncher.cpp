#include "TerminalLauncher.h"

#include <QProcess>
#include <QStandardPaths>

namespace fc {

QVector<TerminalCandidate> terminalCandidates(const QString &workingDirectory) {
#ifdef Q_OS_WIN
    return {
        // Windows Terminal opens each tab in its profile's configured starting
        // folder and ignores the working directory it was launched with, so it
        // is the one that has to be told. It also ships as an App Execution
        // Alias -- a zero-byte reparse point on PATH -- which is why the caller
        // must treat "found it" and "it started" as separate questions.
        {QStringLiteral("wt.exe"), {QStringLiteral("-d"), workingDirectory}},
        {QStringLiteral("pwsh.exe"), {}},
        {QStringLiteral("powershell.exe"), {}},
        // Always present on Windows, which makes it the backstop: this list
        // cannot come up empty on a working system.
        {QStringLiteral("cmd.exe"), {}},
    };
#else
    Q_UNUSED(workingDirectory);
    return {
        // deepin/UOS first, and by name rather than through the
        // x-terminal-emulator alternative: on a deepin desktop that alternative
        // is not guaranteed to point at deepin-terminal, and opening GNOME's or
        // KDE's terminal on a deepin system is exactly the kind of mismatch a
        // user notices. deepin-terminal-gtk is the pre-Qt build, still present
        // on older installs.
        {QStringLiteral("deepin-terminal"), {}},
        {QStringLiteral("deepin-terminal-gtk"), {}},
        // The Debian alternative, which is the right answer on anything that is
        // not deepin and has a desktop-provided default.
        {QStringLiteral("x-terminal-emulator"), {}},
        {QStringLiteral("gnome-terminal"), {}},
        {QStringLiteral("konsole"), {}},
        {QStringLiteral("xfce4-terminal"), {}},
        // Last: it exists nearly everywhere and looks like nothing else on the
        // desktop, so it is a fallback rather than a choice.
        {QStringLiteral("xterm"), {}},
    };
#endif
}

bool openTerminalAt(const QString &workingDirectory) {
    for (const TerminalCandidate &candidate : terminalCandidates(workingDirectory)) {
        if (QStandardPaths::findExecutable(candidate.program).isEmpty())
            continue;
        // Keep going when one refuses to start rather than reporting failure
        // from the first candidate: an execution alias can be on PATH and still
        // fail to launch, and the next entry usually works.
        if (QProcess::startDetached(candidate.program, candidate.arguments, workingDirectory))
            return true;
    }
    return false;
}

} // namespace fc
