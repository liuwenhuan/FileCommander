#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>

#include "update/Updater.h"

// The Windows half of an update runs after the application it is replacing has
// exited, which is exactly why it needs tests of its own: by the time anything
// goes wrong there is no UI left to report it, and the only evidence is
// whatever the script wrote to disk.
//
// These run the real script -- Updater::windowsInstallScript(), the same bytes
// shipped to a user -- against a scratch tree, so the thing under test is the
// PowerShell itself and not a description of it.
namespace {

int runPowerShell(const QStringList &args, QString *output = nullptr, int timeoutMs = 180000) {
    QProcess ps;
    ps.setProcessChannelMode(QProcess::MergedChannels);
    ps.start(QStringLiteral("powershell.exe"),
             QStringList{QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"),
                         QStringLiteral("Bypass")}
                 + args);
    if (!ps.waitForStarted(15000))
        return -1000;
    if (!ps.waitForFinished(timeoutMs)) {
        ps.kill();
        ps.waitForFinished(5000);
        return -1001;
    }
    if (output)
        *output = QString::fromLocal8Bit(ps.readAll());
    return ps.exitCode();
}

bool writeText(const QString &path, const QString &text) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(text.toUtf8()) == text.toUtf8().size();
}

QString readText(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(file.readAll());
}

// A stand-in for FileCommander.exe: a batch file that records the fact it ran.
// Using a real executable means the script's Start-Process relaunch is exercised
// rather than assumed, and the marker file proves it.
const char kExecutableName[] = "fake-app.cmd";

QString appScript(const QString &marker, const QString &tag) {
    return QStringLiteral("@echo off\r\n>>\"%1\" echo %2\r\n").arg(marker, tag);
}

class WindowsUpdateInstallerTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(m_dir.isValid());
        m_root = QDir(m_dir.path()).filePath(QStringLiteral("staging"));
        m_target = QDir(m_dir.path()).filePath(QStringLiteral("install"));
        m_marker = QDir(m_dir.path()).filePath(QStringLiteral("launched.txt"));
        ASSERT_TRUE(QDir().mkpath(m_root));
        ASSERT_TRUE(QDir().mkpath(m_target));

        // The install as it exists before the update: the old executable plus a
        // subdirectory of support files, which is what a real layout looks like.
        ASSERT_TRUE(writeText(liveExecutable(), appScript(m_marker, QStringLiteral("old"))));
        ASSERT_TRUE(writeText(QDir(m_target).filePath(QStringLiteral("platforms/qwindows.dll")),
                              QStringLiteral("old-plugin")));
        ASSERT_TRUE(writeText(QDir(m_target).filePath(QStringLiteral("settings.ini")),
                              QStringLiteral("user data that predates the update")));

        m_scriptPath = QDir(m_root).filePath(QStringLiteral("apply-update.ps1"));
        QFile script(m_scriptPath);
        ASSERT_TRUE(script.open(QIODevice::WriteOnly));
        ASSERT_GT(script.write(Updater::windowsInstallScript()), 0);
        script.close();
    }

    QString liveExecutable() const {
        return QDir(m_target).filePath(QString::fromLatin1(kExecutableName));
    }

    // Builds package.zip from a directory tree, with the payload either at the
    // archive root or inside one wrapper directory -- both layouts the script
    // claims to accept.
    QString buildArchive(bool wrapped, bool includeExecutable = true) {
        const QString content = QDir(m_dir.path()).filePath(QStringLiteral("content"));
        const QString payload =
            wrapped ? QDir(content).filePath(QStringLiteral("FileCommander-windows-x64")) : content;
        if (includeExecutable
            && !writeText(QDir(payload).filePath(QString::fromLatin1(kExecutableName)),
                          appScript(m_marker, QStringLiteral("new"))))
            return QString();
        if (!writeText(QDir(payload).filePath(QStringLiteral("platforms/qwindows.dll")),
                       QStringLiteral("new-plugin")))
            return QString();
        if (!writeText(QDir(payload).filePath(QStringLiteral("README.txt")),
                       QStringLiteral("shipped with the new version")))
            return QString();

        const QString archive = QDir(m_root).filePath(QStringLiteral("package.zip"));
        QString out;
        const int rc = runPowerShell(
            {QStringLiteral("-Command"),
             QStringLiteral("Compress-Archive -Path (Join-Path '%1' '*') -DestinationPath '%2' -Force")
                 .arg(content, archive)},
            &out);
        if (rc != 0) {
            ADD_FAILURE() << "Compress-Archive failed: " << out.toStdString();
            return QString();
        }
        return archive;
    }

    int runInstaller(const QString &archive, qint64 pid, int waitSeconds = 30,
                     QString *output = nullptr) {
        return runPowerShell({QStringLiteral("-File"), m_scriptPath,
                              QStringLiteral("-ProcessId"), QString::number(pid),
                              QStringLiteral("-Archive"), archive,
                              QStringLiteral("-Target"), m_target,
                              QStringLiteral("-Executable"), QString::fromLatin1(kExecutableName),
                              QStringLiteral("-Root"), m_root,
                              QStringLiteral("-WaitSeconds"), QString::number(waitSeconds)},
                             output);
    }

    // A pid that has certainly exited, so the script's wait loop falls straight
    // through. Reusing our own pid would deadlock the test against itself.
    static qint64 anExitedPid() {
        QProcess dead;
        dead.start(QStringLiteral("cmd.exe"), {QStringLiteral("/c"), QStringLiteral("exit")});
        if (!dead.waitForStarted(10000))
            return 0;
        const qint64 pid = dead.processId();
        dead.waitForFinished(10000);
        return pid;
    }

    QTemporaryDir m_dir;
    QString m_root;
    QString m_target;
    QString m_marker;
    QString m_scriptPath;
};

} // namespace

TEST_F(WindowsUpdateInstallerTest, ReplacesTheInstallAndRelaunchesFromAWrappedArchive) {
    const QString archive = buildArchive(/*wrapped=*/true);
    ASSERT_FALSE(archive.isEmpty());

    QString output;
    const int rc = runInstaller(archive, anExitedPid(), 30, &output);
    ASSERT_EQ(rc, 0) << output.toStdString();

    EXPECT_TRUE(readText(liveExecutable()).contains(QStringLiteral("new")))
        << "the executable was not replaced";
    EXPECT_EQ(readText(QDir(m_target).filePath(QStringLiteral("platforms/qwindows.dll"))),
              QStringLiteral("new-plugin"))
        << "files inside an existing subdirectory were not updated";
    EXPECT_TRUE(QFile::exists(QDir(m_target).filePath(QStringLiteral("README.txt"))))
        << "a file new in this release was not delivered";
    // Anything the update does not ship stays put: replacing the folder wholesale
    // would take the user's own files with it.
    EXPECT_EQ(readText(QDir(m_target).filePath(QStringLiteral("settings.ini"))),
              QStringLiteral("user data that predates the update"));

    // The relaunch is part of the contract -- the app quit itself expecting to
    // be started again.
    for (int i = 0; i < 100 && !QFile::exists(m_marker); ++i)
        QThread::msleep(50);
    EXPECT_TRUE(readText(m_marker).contains(QStringLiteral("new")))
        << "the new version was not started";

    // The staging directory is the update's only footprint; a clean run removes it.
    EXPECT_FALSE(QDir(m_root).exists()) << "staging left behind after a successful update";
}

TEST_F(WindowsUpdateInstallerTest, AcceptsAnArchiveWithThePayloadAtItsRoot) {
    const QString archive = buildArchive(/*wrapped=*/false);
    ASSERT_FALSE(archive.isEmpty());

    QString output;
    ASSERT_EQ(runInstaller(archive, anExitedPid(), 30, &output), 0) << output.toStdString();
    EXPECT_TRUE(readText(liveExecutable()).contains(QStringLiteral("new")));
}

// If the archive is not what we think it is, the install must not go ahead --
// and must leave the user with a working application and a record of why.
TEST_F(WindowsUpdateInstallerTest, AnArchiveWithoutTheExecutableIsRefusedAndTheOldAppStaysUsable) {
    const QString archive = buildArchive(/*wrapped=*/true, /*includeExecutable=*/false);
    ASSERT_FALSE(archive.isEmpty());

    QString output;
    EXPECT_NE(runInstaller(archive, anExitedPid(), 30, &output), 0) << output.toStdString();

    EXPECT_TRUE(readText(liveExecutable()).contains(QStringLiteral("old")))
        << "a refused update still overwrote the executable";
    EXPECT_EQ(readText(QDir(m_target).filePath(QStringLiteral("platforms/qwindows.dll"))),
              QStringLiteral("old-plugin"));

    // The log is the only channel left once the app has exited, so it has to
    // survive -- which means the staging directory must NOT be swept up here.
    const QString log = readText(QDir(m_root).filePath(QStringLiteral("update.log")));
    EXPECT_TRUE(log.contains(QStringLiteral("FAILED"))) << log.toStdString();
    EXPECT_TRUE(log.contains(QStringLiteral("does not contain the application executable")))
        << log.toStdString();

    for (int i = 0; i < 100 && !QFile::exists(m_marker); ++i)
        QThread::msleep(50);
    EXPECT_TRUE(readText(m_marker).contains(QStringLiteral("old")))
        << "the user was left with no running application";
}

// The wait loop is bounded on purpose: a previous instance that never exits
// keeps its files locked, so spinning forever would just hide the problem.
TEST_F(WindowsUpdateInstallerTest, GivesUpWhenThePreviousInstanceNeverExits) {
    const QString archive = buildArchive(/*wrapped=*/true);
    ASSERT_FALSE(archive.isEmpty());

    QProcess lingering;
    lingering.start(QStringLiteral("powershell.exe"),
                    {QStringLiteral("-NoProfile"), QStringLiteral("-Command"),
                     QStringLiteral("Start-Sleep -Seconds 45")});
    ASSERT_TRUE(lingering.waitForStarted(15000));

    QString output;
    const int rc = runInstaller(archive, lingering.processId(), /*waitSeconds=*/2, &output);

    lingering.kill();
    lingering.waitForFinished(10000);

    EXPECT_NE(rc, 0) << output.toStdString();
    EXPECT_TRUE(readText(liveExecutable()).contains(QStringLiteral("old")))
        << "the install went ahead while the old instance was still running";
    const QString log = readText(QDir(m_root).filePath(QStringLiteral("update.log")));
    EXPECT_TRUE(log.contains(QStringLiteral("still running"))) << log.toStdString();
}
