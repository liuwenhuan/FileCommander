#include <gtest/gtest.h>

#include <QDir>
#include <QStandardPaths>

#include "TerminalLauncher.h"

// "Open Terminal Here" reported "no terminal emulator found" on every Windows
// machine, because the candidate list was the Linux one and nothing on it
// exists there. The list is therefore what these tests are about: not that a
// terminal opens (that needs a desktop), but that this platform has at least
// one candidate which is actually present.
namespace {

const QString kSomeDirectory = QDir::homePath();

bool installed(const fc::TerminalCandidate &candidate) {
    return !QStandardPaths::findExecutable(candidate.program).isEmpty();
}

QStringList programsIn(const QVector<fc::TerminalCandidate> &candidates) {
    QStringList programs;
    for (const fc::TerminalCandidate &candidate : candidates)
        programs << candidate.program;
    return programs;
}

} // namespace

TEST(TerminalLauncherTest, OffersAtLeastOneTerminalThatExistsOnThisMachine) {
    const QVector<fc::TerminalCandidate> candidates = fc::terminalCandidates(kSomeDirectory);
    ASSERT_FALSE(candidates.isEmpty());

    QStringList tried;
    bool anyInstalled = false;
    for (const fc::TerminalCandidate &candidate : candidates) {
        tried << candidate.program;
        if (installed(candidate))
            anyInstalled = true;
    }

#ifndef Q_OS_WIN
    // A headless Linux box -- a container, a WSL image, a build agent -- has no
    // terminal emulator installed and is not supposed to. That is a fact about
    // the machine, not a defect in the list, so it is a skip rather than a
    // failure. Windows has no such excuse: cmd.exe is always there.
    if (!anyInstalled)
        GTEST_SKIP() << "no terminal emulator installed here: "
                     << tried.join(QLatin1String(", ")).toStdString();
#endif
    EXPECT_TRUE(anyInstalled)
        << "none of these exists here, so the feature can only ever report failure: "
        << tried.join(QLatin1String(", ")).toStdString();
}

#ifdef Q_OS_WIN
// cmd.exe is on every Windows install, which is what makes the list unable to
// come up empty. If it is ever dropped, the guarantee goes with it.
TEST(TerminalLauncherTest, WindowsAlwaysEndsWithABackstopThatCannotBeMissing) {
    const QVector<fc::TerminalCandidate> candidates = fc::terminalCandidates(kSomeDirectory);
    ASSERT_FALSE(candidates.isEmpty());
    EXPECT_EQ(candidates.last().program, QStringLiteral("cmd.exe"));
    EXPECT_TRUE(installed(candidates.last()));
}

// Windows Terminal starts each tab in its profile's configured folder and
// ignores the working directory it was launched with, so it is the one
// candidate that has to carry the directory in its arguments.
TEST(TerminalLauncherTest, WindowsTerminalIsToldTheDirectoryExplicitly) {
    const QString directory = QStringLiteral("C:/some/where");
    const QVector<fc::TerminalCandidate> candidates = fc::terminalCandidates(directory);

    bool sawWindowsTerminal = false;
    for (const fc::TerminalCandidate &candidate : candidates) {
        if (candidate.program != QStringLiteral("wt.exe"))
            continue;
        sawWindowsTerminal = true;
        EXPECT_EQ(candidate.arguments, (QStringList{QStringLiteral("-d"), directory}));
    }
    EXPECT_TRUE(sawWindowsTerminal);
}

// Every other candidate inherits the working directory, so passing it twice
// would be wrong rather than merely redundant -- cmd.exe reads a bare argument
// as a command to run.
TEST(TerminalLauncherTest, ConsoleHostsAreLaunchedWithNoArgumentsAtAll) {
    const QVector<fc::TerminalCandidate> candidates =
        fc::terminalCandidates(QStringLiteral("C:/some/where"));
    for (const fc::TerminalCandidate &candidate : candidates) {
        if (candidate.program == QStringLiteral("wt.exe"))
            continue;
        SCOPED_TRACE(candidate.program.toStdString());
        EXPECT_TRUE(candidate.arguments.isEmpty());
    }
}
#else
// "Open Terminal Here" always opened deepin-terminal no matter what the user
// had configured, because the list named terminals ahead of the Debian
// alternative. The alternative is the entry that asks the desktop instead of
// guessing -- on deepin it resolves to dde-daemon's default-terminal shim,
// which dispatches to the configured default-applications terminal -- so it has
// to come first for the setting to be honoured at all.
TEST(TerminalLauncherTest, TheDesktopsConfiguredTerminalOutranksAnyNamedOne) {
    const QStringList programs = programsIn(fc::terminalCandidates(kSomeDirectory));
    ASSERT_FALSE(programs.isEmpty());
    EXPECT_EQ(programs.first(), QStringLiteral("x-terminal-emulator"));
    EXPECT_LT(programs.indexOf(QStringLiteral("x-terminal-emulator")),
              programs.indexOf(QStringLiteral("deepin-terminal")));
}

// The named entries are only for systems that register no alternative, and on
// such a deepin box the terminal that belongs there is still deepin's own.
TEST(TerminalLauncherTest, DeepinTerminalLeadsTheFallbacks) {
    const QStringList programs = programsIn(fc::terminalCandidates(kSomeDirectory));
    // The pre-Qt build, still present on older deepin installs.
    ASSERT_TRUE(programs.contains(QStringLiteral("deepin-terminal-gtk")));
    for (const QString &other : {QStringLiteral("gnome-terminal"), QStringLiteral("konsole"),
                                 QStringLiteral("xfce4-terminal")}) {
        SCOPED_TRACE(other.toStdString());
        ASSERT_TRUE(programs.contains(other));
        EXPECT_LT(programs.indexOf(QStringLiteral("deepin-terminal")), programs.indexOf(other));
    }
}

// $TERMINAL is the user saying it outright rather than through a desktop
// setting, so it outranks even the alternative -- but only when it names
// something that exists, or the list would lead with a value that can never
// start.
TEST(TerminalLauncherTest, AnExplicitTerminalEnvironmentVariableComesFirst) {
    const bool hadTerminal = qEnvironmentVariableIsSet("TERMINAL");
    const QByteArray previous = qgetenv("TERMINAL");

    qputenv("TERMINAL", QByteArrayLiteral("sh"));
    EXPECT_EQ(programsIn(fc::terminalCandidates(kSomeDirectory)).first(), QStringLiteral("sh"));

    qputenv("TERMINAL", QByteArrayLiteral("fc-no-such-terminal"));
    const QStringList withNonsense = programsIn(fc::terminalCandidates(kSomeDirectory));
    EXPECT_FALSE(withNonsense.contains(QStringLiteral("fc-no-such-terminal")));
    EXPECT_EQ(withNonsense.first(), QStringLiteral("x-terminal-emulator"));

    if (hadTerminal)
        qputenv("TERMINAL", previous);
    else
        qunsetenv("TERMINAL");
}

TEST(TerminalLauncherTest, LinuxKeepsTheDesktopTerminalsAheadOfTheFallback) {
    const QStringList programs = programsIn(fc::terminalCandidates(kSomeDirectory));

    // xterm exists almost everywhere and looks nothing like the desktop's own
    // terminal, so it has to stay last.
    ASSERT_TRUE(programs.contains(QStringLiteral("xterm")));
    EXPECT_EQ(programs.last(), QStringLiteral("xterm"));
    EXPECT_LT(programs.indexOf(QStringLiteral("deepin-terminal")),
              programs.indexOf(QStringLiteral("xterm")));
}

// Every Linux terminal here takes its starting directory from the process it
// was launched by, so passing one as an argument would be wrong rather than
// redundant -- most of them read a bare argument as a command to run.
TEST(TerminalLauncherTest, LinuxTerminalsInheritTheDirectoryRatherThanBeingToldIt) {
    for (const fc::TerminalCandidate &candidate :
         fc::terminalCandidates(QStringLiteral("/some/where"))) {
        SCOPED_TRACE(candidate.program.toStdString());
        EXPECT_TRUE(candidate.arguments.isEmpty());
    }
}
#endif
