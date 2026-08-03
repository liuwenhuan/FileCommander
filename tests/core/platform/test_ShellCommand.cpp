#include <gtest/gtest.h>

#include <QDir>
#include <QProcess>
#include <QStandardPaths>

#include "ShellCommand.h"

// The command bar answered every command with "failed or crashed" on Windows,
// because it started /bin/sh -- a path that does not exist there. The symptom
// the user hit was `dir`, which makes it look like a missing-executable problem
// and is really two: no shell at all, and `dir` being a cmd.exe built-in that
// never exists as an executable in the first place.
namespace {

QString runThroughShell(const QString &command, int *exitCode = nullptr) {
    const fc::ShellInvocation shell = fc::shellInvocationFor(command);
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.setWorkingDirectory(QDir::tempPath());
    process.start(shell.program, shell.arguments);
    if (!process.waitForStarted(15000))
        return QStringLiteral("<did not start>");
    if (!process.waitForFinished(30000)) {
        process.kill();
        return QStringLiteral("<did not finish>");
    }
    if (exitCode)
        *exitCode = process.exitCode();
    return QString::fromLocal8Bit(process.readAll());
}

} // namespace

TEST(ShellCommandTest, NamesAShellThatActuallyExistsOnThisPlatform) {
    const fc::ShellInvocation shell = fc::shellInvocationFor(QStringLiteral("echo hi"));
    ASSERT_FALSE(shell.program.isEmpty());
    EXPECT_FALSE(QStandardPaths::findExecutable(shell.program).isEmpty())
        << shell.program.toStdString()
        << " is not on this system, so every command typed here can only fail";
}

// The command has to reach the shell as ONE argument. Splitting it would break
// the moment anything is quoted, and quoting is most of why running through a
// shell is worth doing.
TEST(ShellCommandTest, PassesTheWholeCommandLineAsASingleArgument) {
    const QString command = QStringLiteral("echo \"one two\" && echo three");
    const fc::ShellInvocation shell = fc::shellInvocationFor(command);
    ASSERT_EQ(shell.arguments.size(), 2);
    EXPECT_EQ(shell.arguments.at(1), command);
#ifdef Q_OS_WIN
    EXPECT_EQ(shell.program, QStringLiteral("cmd.exe"));
    EXPECT_EQ(shell.arguments.at(0), QStringLiteral("/c"));
#else
    EXPECT_EQ(shell.program, QStringLiteral("/bin/sh"));
    EXPECT_EQ(shell.arguments.at(0), QStringLiteral("-c"));
#endif
}

// End to end, because the point of the fix is that a real command produces real
// output rather than "failed or crashed".
TEST(ShellCommandTest, RunsAPlainCommandAndCapturesItsOutput) {
    int exitCode = -1;
    const QString output = runThroughShell(QStringLiteral("echo filecommander"), &exitCode);
    EXPECT_EQ(exitCode, 0) << output.toStdString();
    EXPECT_TRUE(output.contains(QStringLiteral("filecommander"))) << output.toStdString();
}

// A shell built-in is the case that started this: it exists only inside the
// interpreter, so anything that starts processes directly can never run it.
TEST(ShellCommandTest, RunsAShellBuiltInThatIsNotAnExecutable) {
    // Something the interpreter itself must expand -- there is no executable
    // anywhere that could stand in for it. (`pwd` would be the obvious pick on
    // Linux and is the wrong one: /usr/bin/pwd exists, so it would pass even
    // without a shell.)
#ifdef Q_OS_WIN
    const QString builtIn = QStringLiteral("dir");
    const QString firstWord = QStringLiteral("dir");
#else
    const QString builtIn = QStringLiteral("echo $((6*7))");
    const QString firstWord = QStringLiteral("echo");
#endif
    Q_UNUSED(firstWord);
#ifdef Q_OS_WIN
    EXPECT_TRUE(QStandardPaths::findExecutable(builtIn).isEmpty())
        << builtIn.toStdString()
        << " turns out to be a real executable here, so this test proves nothing";
#endif

    int exitCode = -1;
    const QString output = runThroughShell(builtIn, &exitCode);
    EXPECT_EQ(exitCode, 0) << output.toStdString();
    EXPECT_FALSE(output.trimmed().isEmpty()) << "the built-in produced nothing";
#ifndef Q_OS_WIN
    // Arithmetic expansion is the shell's own; a bare exec of "echo" would
    // print the text back verbatim.
    EXPECT_TRUE(output.contains(QStringLiteral("42"))) << output.toStdString();
#endif
}

// Pipes and redirection are the reason the command bar goes through a shell at
// all; the comment in runCommand promised them long before Windows had one.
TEST(ShellCommandTest, HonoursPipesAndQuoting) {
#ifdef Q_OS_WIN
    const QString command = QStringLiteral("echo alpha beta | findstr beta");
#else
    const QString command = QStringLiteral("echo alpha beta | grep beta");
#endif
    int exitCode = -1;
    const QString output = runThroughShell(command, &exitCode);
    EXPECT_EQ(exitCode, 0) << output.toStdString();
    EXPECT_TRUE(output.contains(QStringLiteral("beta"))) << output.toStdString();
}
