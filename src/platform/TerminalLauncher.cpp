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
    QVector<TerminalCandidate> candidates = {
        // The Debian alternative first, because it is the only entry that asks
        // the desktop rather than guessing. On deepin it resolves to
        // dde-daemon's default-terminal shim, which reads the user's
        // com.deepin.desktop.default-applications.terminal setting and
        // dispatches to whatever that names -- so honouring the alternative is
        // how the configured terminal gets honoured. Naming terminals ahead of
        // it, as this list used to, is what made "Open Terminal Here" jump over
        // the setting and always open deepin-terminal.
        {QStringLiteral("x-terminal-emulator"), {}},
        // Everything below is for systems that register no alternative. deepin's
        // own terminal stays ahead of GNOME's and KDE's so a deepin box missing
        // the alternative still lands on the terminal that belongs there;
        // deepin-terminal-gtk is the pre-Qt build, still present on older
        // installs.
        {QStringLiteral("deepin-terminal"), {}},
        {QStringLiteral("deepin-terminal-gtk"), {}},
        {QStringLiteral("gnome-terminal"), {}},
        {QStringLiteral("konsole"), {}},
        {QStringLiteral("xfce4-terminal"), {}},
        // Last: it exists nearly everywhere and looks like nothing else on the
        // desktop, so it is a fallback rather than a choice.
        {QStringLiteral("xterm"), {}},
    };
    // An explicit $TERMINAL is the user saying it outright, so nothing outranks
    // it. Checked against PATH here rather than left for the caller to skip, so
    // a stale value cannot sit at the head of the list pretending to be the
    // answer.
    const QString preferred = qEnvironmentVariable("TERMINAL");
    if (!preferred.isEmpty() && !QStandardPaths::findExecutable(preferred).isEmpty())
        candidates.prepend({preferred, {}});
    return candidates;
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
