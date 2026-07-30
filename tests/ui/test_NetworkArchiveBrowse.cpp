#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QVector>

#include <memory>

#include "ArchiveHandler.h"
#include "FilePanel.h"
#include "FileProvider.h"
#include "FileSystemModel.h"

// Browsing an archive that lives on a share.
//
// The archive itself is always read off local disk -- libarchive opens a file by
// name -- so what is actually at stake here is everything AROUND that: the panel
// has to hold the server connection open across a browse that does not use it,
// put it back when the user steps out, and land them in the directory on the
// server rather than in the /tmp directory the copy happens to sit in. Those are
// the parts that were wrong (or absent) before, and none of them involves the
// download, so the tests drive enterArchive() directly with a copy already in
// place -- exactly what MainWindow::browseRemoteArchive hands it.
namespace {

// A stand-in server: one directory holding one archive. Everything is fixed and
// immutable, which is what makes it safe to hand to NetworkSession's worker.
class FakeShare : public FileProvider {
public:
    FakeShare(QString dir, QString archiveName, qint64 archiveSize)
        : m_dir(std::move(dir)), m_archive(std::move(archiveName)), m_size(archiveSize) {}

    QString displayName() const override { return QStringLiteral("tester@share"); }
    QString scheme() const override { return QStringLiteral("smb"); }

    QVector<FileInfo> list(const QString &path, bool) const override {
        QVector<FileInfo> out;
        if (cleanPath(path) != m_dir)
            return out;
        out.append(FileInfo::fromFields(m_dir + QLatin1Char('/') + m_archive, m_archive, m_size,
                                        QDateTime::currentDateTime(), false, QFile::ReadOwner));
        return out;
    }
    bool isDir(const QString &path) const override { return cleanPath(path) == m_dir; }
    QString cleanPath(const QString &path) const override {
        QString p = path;
        while (p.size() > 1 && p.endsWith(QLatin1Char('/')))
            p.chop(1);
        return p;
    }
    QString parentPath(const QString &path) const override {
        const QString clean = cleanPath(path);
        const int slash = clean.lastIndexOf(QLatin1Char('/'));
        if (slash < 0)
            return {};
        return slash == 0 ? QStringLiteral("/") : clean.left(slash);
    }
    bool exists(const QString &path) const override {
        const QString clean = cleanPath(path);
        return clean == m_dir || clean == m_dir + QLatin1Char('/') + m_archive;
    }
    RenameResult rename(const QString &, const QString &, QString *) override {
        return RenameResult::Failed;
    }

private:
    QString m_dir;
    QString m_archive;
    qint64 m_size;
};

// Waits for the panel's asynchronous directory load to land.
void settle(FilePanel &panel) {
    QSignalSpy spy(panel.model(), &FileSystemModel::loadFinished);
    if (spy.isEmpty())
        spy.wait(4000);
    QCoreApplication::processEvents();
}

void navigate(FilePanel &panel, const QString &path) {
    QSignalSpy spy(panel.model(), &FileSystemModel::loadFinished);
    panel.navigateTo(path);
    if (spy.isEmpty())
        spy.wait(4000);
    QCoreApplication::processEvents();
}

bool listHasName(FileSystemModel *model, const QString &name) {
    for (int row = 0; row < model->rowCount(); ++row)
        if (model->fileInfoAt(row).name() == name)
            return true;
    return false;
}

// Builds pkg/{greeting.txt, nested/deep.txt} as a zip at `outPath`.
bool buildZip(const QString &srcRoot, const QString &outPath) {
    const QString pkg = QDir(srcRoot).filePath("pkg");
    if (!QDir().mkpath(QDir(pkg).filePath("nested")))
        return false;
    QFile a(QDir(pkg).filePath("greeting.txt"));
    if (!a.open(QIODevice::WriteOnly))
        return false;
    a.write("hello zip");
    a.close();
    QFile b(QDir(pkg).filePath("nested/deep.txt"));
    if (!b.open(QIODevice::WriteOnly))
        return false;
    b.write("deep content");
    b.close();
    QString err;
    return ArchiveHandler::create(outPath, {pkg}, QStringLiteral("zip"), &err);
}

} // namespace

// The regression guard for the case that already worked: a local archive is
// entered in place, ".." returns to the directory holding it, and the user's own
// file is still there afterwards.
TEST(NetworkArchiveBrowse, LocalArchiveEntersAndExitsUnchanged) {
    QTemporaryDir srcDir, hostDir;
    ASSERT_TRUE(srcDir.isValid() && hostDir.isValid());
    const QString archivePath = QDir(hostDir.path()).filePath("bundle.zip");
    ASSERT_TRUE(buildZip(srcDir.path(), archivePath));

    FilePanel panel;
    navigate(panel, hostDir.path());
    ASSERT_FALSE(panel.isArchive());

    ASSERT_TRUE(panel.enterArchive(archivePath, archivePath, false));
    settle(panel);
    EXPECT_TRUE(panel.isArchive());
    EXPECT_EQ(panel.currentPath(), QString("/"));
    EXPECT_TRUE(listHasName(panel.model(), "greeting.txt"));
    EXPECT_TRUE(listHasName(panel.model(), "nested"));

    panel.navigateUp(); // the ".." row, i.e. what a double-click on it does
    settle(panel);
    EXPECT_FALSE(panel.isArchive());
    EXPECT_EQ(panel.currentPath(), hostDir.path());
    EXPECT_TRUE(QFile::exists(archivePath)) << "browsing a local archive must not delete it";
}

// The archive is on the share, so the copy is in /tmp -- but ".." has to lead
// back to the share, with the connection that serves it still up.
TEST(NetworkArchiveBrowse, RemoteArchiveExitsToTheShareWithTheConnectionIntact) {
    QTemporaryDir srcDir, copyRoot;
    ASSERT_TRUE(srcDir.isValid() && copyRoot.isValid());
    // The downloaded copy lands in a directory of its own, as the fetch arranges.
    const QString copyDir = QDir(copyRoot.path()).filePath("7");
    ASSERT_TRUE(QDir().mkpath(copyDir));
    const QString localCopy = QDir(copyDir).filePath("bundle.zip");
    ASSERT_TRUE(buildZip(srcDir.path(), localCopy));

    const QString remoteDir = QStringLiteral("/share/docs");
    const QString remotePath = remoteDir + QStringLiteral("/bundle.zip");

    FilePanel panel;
    auto share = std::make_shared<FakeShare>(remoteDir, QStringLiteral("bundle.zip"),
                                             QFileInfo(localCopy).size());
    panel.connectTabTo(0, share, [](QString *) { return true; }, remoteDir,
                       QStringLiteral("tester@share"), SavedConnection{},
                       FileSystemModel::AuthRetryFactory());
    settle(panel);
    ASSERT_TRUE(panel.model()->hasNetworkSession());
    ASSERT_TRUE(listHasName(panel.model(), "bundle.zip"));
    const QString connId = panel.connectionId();
    ASSERT_FALSE(connId.isEmpty());

    ASSERT_TRUE(panel.enterArchive(localCopy, remotePath, true));
    settle(panel);
    EXPECT_TRUE(panel.isArchive());
    EXPECT_TRUE(listHasName(panel.model(), "greeting.txt"));
    // The connection is parked, not torn down: it is not the active backend
    // (the archive is), but it is still running and still owned.
    EXPECT_FALSE(panel.model()->hasNetworkSession());

    panel.navigateUp();
    settle(panel);
    EXPECT_FALSE(panel.isArchive());
    EXPECT_EQ(panel.currentPath(), remoteDir) << "'..' must return to the share, not to /tmp";
    EXPECT_TRUE(panel.model()->hasNetworkSession());
    EXPECT_EQ(panel.connectionId(), connId);
    EXPECT_TRUE(listHasName(panel.model(), "bundle.zip"));

    // The copy existed only for this browse.
    EXPECT_FALSE(QFile::exists(localCopy));
    EXPECT_FALSE(QDir(copyDir).exists());
}

// Switching tabs takes the panel's backend with it, so the archive cannot stay:
// the tab goes back to the directory it was entered from and keeps its server.
TEST(NetworkArchiveBrowse, TabSwitchBacksOutOfTheArchiveKeepingTheConnection) {
    QTemporaryDir srcDir, copyRoot;
    ASSERT_TRUE(srcDir.isValid() && copyRoot.isValid());
    const QString localCopy = QDir(copyRoot.path()).filePath("bundle.zip");
    ASSERT_TRUE(buildZip(srcDir.path(), localCopy));

    const QString remoteDir = QStringLiteral("/share/docs");

    FilePanel panel;
    auto share = std::make_shared<FakeShare>(remoteDir, QStringLiteral("bundle.zip"),
                                             QFileInfo(localCopy).size());
    panel.connectTabTo(0, share, [](QString *) { return true; }, remoteDir,
                       QStringLiteral("tester@share"), SavedConnection{},
                       FileSystemModel::AuthRetryFactory());
    settle(panel);
    ASSERT_TRUE(panel.model()->hasNetworkSession());

    ASSERT_TRUE(panel.enterArchive(localCopy, remoteDir + QStringLiteral("/bundle.zip"), false));
    settle(panel);
    ASSERT_TRUE(panel.isArchive());

    panel.newTab();
    settle(panel);
    EXPECT_FALSE(panel.isArchive());

    panel.prevTab();
    settle(panel);
    EXPECT_EQ(panel.currentPath(), remoteDir);
    EXPECT_TRUE(panel.model()->hasNetworkSession());
    EXPECT_TRUE(listHasName(panel.model(), "bundle.zip"));
}

// Closing the tab an archive was entered from takes the archive with it: the
// panel must not go on reporting itself read-only over whatever tab slides in.
TEST(NetworkArchiveBrowse, ClosingTheArchivesTabLeavesTheArchive) {
    QTemporaryDir srcDir, hostDir;
    ASSERT_TRUE(srcDir.isValid() && hostDir.isValid());
    const QString archivePath = QDir(hostDir.path()).filePath("bundle.zip");
    ASSERT_TRUE(buildZip(srcDir.path(), archivePath));

    FilePanel panel;
    panel.newTab(); // a second tab, so the first one is closable
    panel.prevTab();
    QCoreApplication::processEvents();
    navigate(panel, hostDir.path());
    ASSERT_TRUE(panel.enterArchive(archivePath, archivePath, false));
    settle(panel);
    ASSERT_TRUE(panel.isArchive());

    panel.closeCurrentTab();
    settle(panel);
    EXPECT_FALSE(panel.isArchive());
    EXPECT_NE(panel.currentPath(), QString("/"));
}

// Browsing a remote archive through its gvfs mount instead of a download.
//
// The mount hands back a real path -- /run/user/<uid>/gvfs/<mount>/bundle.zip --
// that reads the file straight off the server, which is why this path exists at
// all: a 686 MB 7z opened in 44 ms through the mount against 10.64 s downloaded.
// What changes structurally is ownership. A downloaded copy is ours and is
// deleted on the way out; a mounted path is the USER'S FILE ON THEIR SERVER, and
// the same deletion there destroys it. The only thing separating the two is the
// ownsLocalCopy=false that MainWindow::browseRemoteArchive passes, so pin it.
TEST(NetworkArchiveBrowse, ArchiveOpenedThroughAMountIsNeverDeleted) {
    QTemporaryDir srcDir, mountRoot;
    ASSERT_TRUE(srcDir.isValid() && mountRoot.isValid());
    // Stands in for the mount point: a path that is real on this machine but
    // whose contents belong to the server, exactly like a gvfs mount's.
    const QString mountedArchive = QDir(mountRoot.path()).filePath("bundle.zip");
    ASSERT_TRUE(buildZip(srcDir.path(), mountedArchive));

    const QString remoteDir = QStringLiteral("/share/docs");
    const QString remotePath = remoteDir + QStringLiteral("/bundle.zip");

    FilePanel panel;
    auto share = std::make_shared<FakeShare>(remoteDir, QStringLiteral("bundle.zip"),
                                             QFileInfo(mountedArchive).size());
    panel.connectTabTo(0, share, [](QString *) { return true; }, remoteDir,
                       QStringLiteral("tester@share"), SavedConnection{},
                       FileSystemModel::AuthRetryFactory());
    settle(panel);
    ASSERT_TRUE(panel.model()->hasNetworkSession());
    const QString connId = panel.connectionId();

    // ownsLocalCopy = false: this is the mount, not a copy of it.
    ASSERT_TRUE(panel.enterArchive(mountedArchive, remotePath, false));
    settle(panel);
    EXPECT_TRUE(panel.isArchive());
    EXPECT_TRUE(listHasName(panel.model(), "greeting.txt"));
    EXPECT_TRUE(listHasName(panel.model(), "nested"));

    panel.navigateUp();
    settle(panel);
    EXPECT_FALSE(panel.isArchive());
    // Same exit behaviour as the downloaded case: back to the server directory,
    // on the connection that was parked, not to wherever the file sat locally.
    EXPECT_EQ(panel.currentPath(), remoteDir);
    EXPECT_TRUE(panel.model()->hasNetworkSession());
    EXPECT_EQ(panel.connectionId(), connId);

    EXPECT_TRUE(QFile::exists(mountedArchive))
        << "a mounted archive is the user's file on their server -- leaving the browse "
           "must never delete it";
    EXPECT_TRUE(QDir(mountRoot.path()).exists()) << "nor take the directory holding it";
}

// The same guarantee where it is easiest to lose: the tab is closed (or switched
// away from) rather than walked out of, so the teardown runs from a different
// path than navigateUp().
TEST(NetworkArchiveBrowse, ClosingTheTabDoesNotDeleteAMountedArchive) {
    QTemporaryDir srcDir, mountRoot;
    ASSERT_TRUE(srcDir.isValid() && mountRoot.isValid());
    const QString mountedArchive = QDir(mountRoot.path()).filePath("bundle.zip");
    ASSERT_TRUE(buildZip(srcDir.path(), mountedArchive));

    FilePanel panel;
    panel.newTab(); // a second tab, so the first one is closable
    panel.prevTab();
    QCoreApplication::processEvents();
    navigate(panel, mountRoot.path());
    ASSERT_TRUE(panel.enterArchive(mountedArchive, QStringLiteral("/share/bundle.zip"), false));
    settle(panel);
    ASSERT_TRUE(panel.isArchive());

    panel.closeCurrentTab();
    settle(panel);
    EXPECT_FALSE(panel.isArchive());
    EXPECT_TRUE(QFile::exists(mountedArchive));
}
