#include <gtest/gtest.h>

#include <memory>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QMessageBox>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUuid>

#include "ConnectionStore.h"
#include "CurlWebDavProvider.h"
#include "FilePanel.h"
#include "FileSystemModel.h"
#include "GvfsMounter.h"
#include "MainWindow.h"
#include "Settings.h"
#include "SmbProvider.h"
#include "ThemeStateGuard.h"

// An archive that lives on a server is still an archive, and the extract
// commands have to reach it: the provider path is not a path on this machine,
// so a command resolves it to the connection's gvfs mount point -- or, where a
// downloaded copy will do, streams it into a temp dir -- before ArchiveHandler
// opens it with QFile. Every step below the resolve (mounting, streaming,
// listing) is only real against a real server, so these are live tests:
//
//   FC_LIVE_CONNECTION_ID=<uuid from the connection manager>   # SMB or WebDAV
//   FC_LIVE_PARENT=/<dir>          # a concrete writable directory on it
//   FILECOMMANDER_CONFIG_HOME=$HOME/.config
//
// The third one is not optional: test_main.cpp turns on QStandardPaths test
// mode, so without it ConnectionStore reads ~/.qttest/config and finds no
// bookmarks at all.
//
// Which of the three tests runs depends on whether the connection can be
// gvfs-mounted right now -- the mounted and unmounted paths are different code
// in MainWindow, and each test skips when it is looking at the other world.
//
// Unmounting the bookmark is NOT enough to reach the unmounted pair: the resolve
// asks GvfsMounter with MountIfNeeded, so it mounts the connection itself from
// the credentials the provider already holds. What is left when that fails is
// the "gvfs may not be installed" case the header of localPathFor() names, so
// that is how the unmounted pair is reached here -- unmount first, then hide the
// tool the mounting shells out to:
//
//   gio mount -u dav://<user>@<host>:<port>/<path>
//   mkdir -p /tmp/fc-nogvfs && printf '#!/bin/sh\nexit 1\n' > /tmp/fc-nogvfs/gio
//   chmod +x /tmp/fc-nogvfs/gio && PATH=/tmp/fc-nogvfs:$PATH ...
//
// Every test creates one freshly named directory under FC_LIVE_PARENT, works
// only inside it, and removes it again. Credentials come from the keyring,
// never the environment.

namespace {

struct LiveProvider {
    std::shared_ptr<FileProvider> provider;
    std::function<bool(QString *)> connect;
};

// Mirrors MainWindow's providerForSaved(): the connect closure is what the
// model's worker thread runs, so the provider it returns must not be connected
// from anywhere else first.
LiveProvider providerForSaved(const SavedConnection &c) {
    const QString password = c.anonymous ? QString() : ConnectionStore::loadPassword(c.id);
    switch (static_cast<ConnectionProtocol>(c.protocol)) {
    case ConnectionProtocol::Smb: {
        auto p = std::make_shared<SmbProvider>();
        p->setTimeoutMs(20000);
        return {p, [p, c, password](QString *e) {
                    return p->connectToHost(c.host, c.user, password, QString(), c.anonymous, e);
                }};
    }
    case ConnectionProtocol::WebDav:
    case ConnectionProtocol::WebDavs: {
        auto p = std::make_shared<CurlWebDavProvider>();
        p->setTimeoutMs(20000);
        const bool useHttps =
            static_cast<ConnectionProtocol>(c.protocol) == ConnectionProtocol::WebDavs;
        return {p, [p, c, password, useHttps](QString *e) {
                    return p->connectToHost(c.host, c.port, c.user, password, useHttps, e);
                }};
    }
    default:
        return {};
    }
}

// What the environment names, connected and checked. `skip` means there is
// nothing to test against; `fatal` means there was and it did not work.
struct LiveTarget {
    SavedConnection saved;
    std::shared_ptr<FileProvider> provider;
    QString parent;
    QString skip;
    QString fatal;
};

LiveTarget openTarget() {
    LiveTarget t;
    const QString id = qEnvironmentVariable("FC_LIVE_CONNECTION_ID");
    t.parent = qEnvironmentVariable("FC_LIVE_PARENT");
    if (id.isEmpty() || t.parent.isEmpty()) {
        t.skip = QStringLiteral("Set FC_LIVE_CONNECTION_ID and FC_LIVE_PARENT to run this test");
        return t;
    }
    if (!t.parent.startsWith(QLatin1Char('/')) || t.parent.contains(QStringLiteral(".."))) {
        t.skip = QStringLiteral("FC_LIVE_PARENT must be an absolute path with no traversal "
                                "segments; refusing to touch the server");
        return t;
    }
    t.saved = ConnectionStore::load(id);
    if (t.saved.id.isEmpty() || t.saved.host.isEmpty()) {
        t.skip = QStringLiteral("FC_LIVE_CONNECTION_ID names no saved connection in %1")
                     .arg(Settings::configFilePath());
        return t;
    }

    LiveProvider live = providerForSaved(t.saved);
    if (!live.provider) {
        t.fatal = QStringLiteral("only SMB and WebDAV bookmarks are supported here");
        return t;
    }
    QString error;
    if (!live.connect(&error)) {
        t.fatal = error;
        return t;
    }
    if (!live.provider->isDir(t.parent)) {
        t.fatal = QStringLiteral("FC_LIVE_PARENT is not a directory on the server");
        return t;
    }
    t.provider = live.provider;
    return t;
}

// One directory of our own on the server, gone again when the test leaves --
// including when it leaves through an ASSERT.
struct RemoteScratch {
    std::shared_ptr<FileProvider> provider;
    QString path;

    RemoteScratch(std::shared_ptr<FileProvider> p, const QString &parent) : provider(std::move(p)) {
        const QString candidate = parent + QStringLiteral("/FC-ARC-") +
                                  QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (provider->mkdir(candidate))
            path = candidate;
    }
    ~RemoteScratch() {
        if (path.isEmpty())
            return;
        for (const FileInfo &entry : provider->list(path, true))
            provider->remove(path + QLatin1Char('/') + entry.name());
        EXPECT_TRUE(provider->remove(path)) << "left " << path.toStdString() << " on the server";
    }
};

// A zip holding one known file, built locally so the fixture's contents are
// known independently of whatever the server already holds.
QString buildFixtureZip(const QString &dir) {
    const QString payload = QDir(dir).filePath(QStringLiteral("payload"));
    if (!QDir().mkpath(payload))
        return {};
    QFile note(QDir(payload).filePath(QStringLiteral("hello.txt")));
    if (!note.open(QIODevice::WriteOnly))
        return {};
    note.write("remote archive fixture\n");
    note.close();

    const QString zipPath = QDir(dir).filePath(QStringLiteral("fixture.zip"));
    QProcess zip;
    zip.setWorkingDirectory(payload);
    zip.start(QStringLiteral("zip"), {"-q", zipPath, "hello.txt"});
    if (!zip.waitForStarted() || !zip.waitForFinished(30000) || zip.exitCode() != 0)
        return {};
    return zipPath;
}

// With no gvfs mount there is no local path to copy the fixture to, so it goes
// up the way the app's own transfers do. The fixture is a few hundred bytes, so
// one write is the whole file.
bool uploadFile(FileProvider *provider, const QString &localPath, const QString &remotePath) {
    QFile in(localPath);
    if (!in.open(QIODevice::ReadOnly))
        return false;
    const QByteArray bytes = in.readAll();
    FileHandle *handle = provider->openWrite(remotePath, true);
    if (!handle)
        return false;
    provider->setExpectedWriteSize(handle, bytes.size());
    const qint64 written = provider->write(handle, bytes.constData(), bytes.size());
    const bool committed = provider->closeHandleStatus(handle);
    return committed && written == bytes.size();
}

int rowFor(FileSystemModel *model, const QString &name) {
    for (int r = 0; r < model->rowCount(); ++r) {
        if (!model->isParentEntry(r) && model->fileInfoAt(r).name() == name)
            return r;
    }
    return -1;
}

// What the panel actually holds, for when the wait below times out.
std::string panelState(FilePanel *panel) {
    QStringList names;
    for (int r = 0; r < panel->model()->rowCount(); ++r)
        names << panel->model()->fileInfoAt(r).name();
    return QStringLiteral("path=%1 sessionState=%2 rows=[%3]")
        .arg(panel->currentPath())
        .arg(panel->model()->sessionState())
        .arg(names.join(QLatin1Char(',')))
        .toStdString();
}

// Puts the panel's first tab on a directory on the server and waits for it to
// list the fixture. The model connects and drives its own provider on a worker
// thread, and the test's is still in use from here, hence a second one.
::testing::AssertionResult showRemoteDir(FilePanel *panel, const SavedConnection &saved,
                                         const QString &remoteDir) {
    LiveProvider live = providerForSaved(saved);
    if (!live.provider)
        return ::testing::AssertionFailure() << "no provider for the saved connection";
    // connectTabTo() rather than the model directly -- it is what also navigates
    // the panel to the remote path.
    panel->connectTabTo(0, live.provider, live.connect, remoteDir, saved.host, saved, {});

    const bool listed = QTest::qWaitFor(
        [panel, &remoteDir] {
            return panel->currentPath() == remoteDir &&
                   rowFor(panel->model(), QStringLiteral("fixture.zip")) >= 0;
        },
        60000);
    if (!listed)
        return ::testing::AssertionFailure()
               << "the panel never listed the archive on the server: " << panelState(panel);

    panel->view()->setCurrentIndex(
        panel->model()->index(rowFor(panel->model(), QStringLiteral("fixture.zip")), 0));
    if (panel->currentEntryPath() != remoteDir + QStringLiteral("/fixture.zip"))
        return ::testing::AssertionFailure()
               << "the archive is not the current entry: " << panelState(panel);
    return ::testing::AssertionSuccess();
}

} // namespace

TEST(RemoteArchiveLiveTest, ExtractHereUnpacksAnArchiveThatLivesOnAServer) {
    LiveTarget target = openTarget();
    if (!target.skip.isEmpty())
        GTEST_SKIP() << target.skip.toStdString();
    ASSERT_TRUE(target.fatal.isEmpty()) << target.fatal.toStdString();

    // The command under test resolves through the connection's gvfs mount;
    // without one it takes the refusal path the next test covers.
    const QString mountedParent = GvfsMounter::localPathFor(target.provider.get(), target.parent);
    if (mountedParent.isEmpty())
        GTEST_SKIP() << "the connection is not gvfs-mounted, so there is no real path to extract to";

    RemoteScratch scratch(target.provider, target.parent);
    ASSERT_FALSE(scratch.path.isEmpty()) << "could not create the test directory on the server";
    const QString mountedDir =
        QDir(mountedParent).filePath(QFileInfo(scratch.path).fileName());

    QTemporaryDir localRoot;
    ASSERT_TRUE(localRoot.isValid());
    const QString fixture = buildFixtureZip(localRoot.path());
    if (fixture.isEmpty())
        GTEST_SKIP() << "zip(1) unavailable";
    ASSERT_TRUE(QFile::copy(fixture, QDir(mountedDir).filePath(QStringLiteral("fixture.zip"))))
        << "could not place the fixture in " << mountedDir.toStdString();

    {
        ThemeStateGuard themeGuard;
        MainWindow window;
        window.show();
        ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));

        FilePanel *panel = window.findChildren<FilePanel *>().value(0);
        ASSERT_NE(panel, nullptr);
        window.setActivePanel(panel);
        ASSERT_TRUE(showRemoteDir(panel, target.saved, scratch.path));

        // Whichever way it ends -- the success report or the refusal warning --
        // the command finishes in a modal dialog, and with no user here nothing
        // else would ever close it. Its text is also the only way to tell the
        // two endings apart from outside.
        QString reported;
        QTimer dismisser;
        QObject::connect(&dismisser, &QTimer::timeout, [&reported] {
            QWidget *modal = QApplication::activeModalWidget();
            if (!modal)
                return;
            if (auto *box = modal->findChild<QMessageBox *>())
                reported = box->text();
            modal->close();
        });
        dismisser.start(200);

        ASSERT_TRUE(QMetaObject::invokeMethod(&window, "extractArchiveHere", Qt::DirectConnection));

        // Both the resolve and the extraction that follows it are asynchronous.
        EXPECT_TRUE(QTest::qWaitFor(
            [&mountedDir] { return QFile::exists(QDir(mountedDir).filePath("hello.txt")); },
            120000))
            << "the archive was never unpacked onto the server; the command said: "
            << reported.toStdString();
        EXPECT_TRUE(QTest::qWaitFor([&reported] { return !reported.isEmpty(); }, 30000))
            << "the command reported nothing";
        EXPECT_TRUE(reported.contains(mountedDir))
            << "expected the extraction to land on the server, but it said: "
            << reported.toStdString();
    }
}

// The other half of the same fix: with no mount there is no path on this
// machine that names the server's directory, and a downloaded copy cannot be
// unpacked back to where it came from -- so "Extract Here" says so instead of
// failing silently or unpacking somewhere the user did not ask for.
TEST(RemoteArchiveLiveTest, ExtractHereRefusesWhenTheServerIsNotMounted) {
    LiveTarget target = openTarget();
    if (!target.skip.isEmpty())
        GTEST_SKIP() << target.skip.toStdString();
    ASSERT_TRUE(target.fatal.isEmpty()) << target.fatal.toStdString();

    if (!GvfsMounter::localPathFor(target.provider.get(), target.parent).isEmpty())
        GTEST_SKIP() << "the connection is gvfs-mounted, so this is the previous test's world";
    ASSERT_TRUE(target.provider->canStream()) << "the fixture can only be placed by streaming";

    RemoteScratch scratch(target.provider, target.parent);
    ASSERT_FALSE(scratch.path.isEmpty()) << "could not create the test directory on the server";

    QTemporaryDir localRoot;
    ASSERT_TRUE(localRoot.isValid());
    const QString fixture = buildFixtureZip(localRoot.path());
    if (fixture.isEmpty())
        GTEST_SKIP() << "zip(1) unavailable";
    ASSERT_TRUE(uploadFile(target.provider.get(), fixture,
                           scratch.path + QStringLiteral("/fixture.zip")))
        << "could not upload the fixture to " << scratch.path.toStdString();

    {
        ThemeStateGuard themeGuard;
        MainWindow window;
        window.show();
        ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));

        FilePanel *panel = window.findChildren<FilePanel *>().value(0);
        ASSERT_NE(panel, nullptr);
        window.setActivePanel(panel);
        ASSERT_TRUE(showRemoteDir(panel, target.saved, scratch.path));

        QString reported;
        QTimer dismisser;
        QObject::connect(&dismisser, &QTimer::timeout, [&reported] {
            QWidget *modal = QApplication::activeModalWidget();
            if (!modal)
                return;
            if (auto *box = modal->findChild<QMessageBox *>())
                reported = box->text();
            modal->close();
        });
        dismisser.start(200);

        ASSERT_TRUE(QMetaObject::invokeMethod(&window, "extractArchiveHere", Qt::DirectConnection));

        ASSERT_TRUE(QTest::qWaitFor([&reported] { return !reported.isEmpty(); }, 60000))
            << "the command neither refused nor reported anything";
        // The refusal names the archive; the success report names a destination
        // directory instead. Comparing that way survives translation.
        EXPECT_TRUE(reported.contains(QStringLiteral("fixture.zip")))
            << "expected a refusal naming the archive, but it said: " << reported.toStdString();
    }

    QStringList left;
    for (const FileInfo &entry : target.provider->list(scratch.path, true))
        left << entry.name();
    EXPECT_EQ(left, QStringList{QStringLiteral("fixture.zip")})
        << "the refusal still wrote to the server";
}

// The download-copy path: "Extract to Folder..." unpacks into a directory the
// user picks on this machine, so nothing has to be written back to the server
// and a streamed copy of the archive is good enough. With no mount that is the
// only way it can work at all.
TEST(RemoteArchiveLiveTest, ExtractToFolderDownloadsACopyWhenTheServerIsNotMounted) {
    LiveTarget target = openTarget();
    if (!target.skip.isEmpty())
        GTEST_SKIP() << target.skip.toStdString();
    ASSERT_TRUE(target.fatal.isEmpty()) << target.fatal.toStdString();

    if (!GvfsMounter::localPathFor(target.provider.get(), target.parent).isEmpty())
        GTEST_SKIP() << "the connection is gvfs-mounted, so the copy would never be downloaded";
    ASSERT_TRUE(target.provider->canStream()) << "the fixture can only be placed by streaming";

    RemoteScratch scratch(target.provider, target.parent);
    ASSERT_FALSE(scratch.path.isEmpty()) << "could not create the test directory on the server";

    QTemporaryDir localRoot;
    ASSERT_TRUE(localRoot.isValid());
    const QString fixture = buildFixtureZip(localRoot.path());
    if (fixture.isEmpty())
        GTEST_SKIP() << "zip(1) unavailable";
    ASSERT_TRUE(uploadFile(target.provider.get(), fixture,
                           scratch.path + QStringLiteral("/fixture.zip")))
        << "could not upload the fixture to " << scratch.path.toStdString();

    QTemporaryDir destination;
    ASSERT_TRUE(destination.isValid());

    {
        ThemeStateGuard themeGuard;
        MainWindow window;
        window.show();
        ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));

        FilePanel *panel = window.findChildren<FilePanel *>().value(0);
        ASSERT_NE(panel, nullptr);
        window.setActivePanel(panel);
        ASSERT_TRUE(showRemoteDir(panel, target.saved, scratch.path));

        // This command opens two modals in a row: the destination picker, and
        // the report when it is done. Both would sit there forever.
        QString reported;
        bool sawPicker = false;
        int idleTicks = 0;
        QTimer driver;
        QObject::connect(&driver, &QTimer::timeout, [&] {
            QWidget *modal = QApplication::activeModalWidget();
            if (auto *picker = qobject_cast<QFileDialog *>(modal)) {
                sawPicker = true;
                // With nothing selected, the dialog answers with the directory
                // it is showing. accept() is protected on QFileDialog, but it
                // is still QDialog's slot.
                picker->setDirectory(destination.path());
                QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection);
                return;
            }
            if (modal) {
                if (auto *box = modal->findChild<QMessageBox *>())
                    reported = box->text();
                modal->close();
                return;
            }
            // The picker is a blocking static call, so if this platform hands it
            // to a native dialog no widget of ours ever appears and nothing here
            // can answer it. Quit its event loop rather than hang the suite; the
            // command then returns with no destination and the test says why.
            if (!sawPicker && ++idleTicks > 75)
                QCoreApplication::exit();
        });
        driver.start(200);

        ASSERT_TRUE(QMetaObject::invokeMethod(&window, "extractArchiveToDir", Qt::DirectConnection));
        ASSERT_TRUE(sawPicker) << "no QFileDialog appeared -- the platform theme supplies a native "
                                  "destination picker this test cannot drive";

        // Streaming the archive down and unpacking it are both asynchronous from
        // here; the file appearing is the only signal that both finished.
        EXPECT_TRUE(QTest::qWaitFor(
            [&destination] {
                return QFile::exists(QDir(destination.path()).filePath("hello.txt"));
            },
            120000))
            << "the downloaded copy was never unpacked into " << destination.path().toStdString()
            << "; the command said: " << reported.toStdString();
        EXPECT_TRUE(QTest::qWaitFor([&reported] { return !reported.isEmpty(); }, 30000))
            << "the command reported nothing";
        EXPECT_TRUE(reported.contains(destination.path()))
            << "expected the extraction to land in the chosen folder, but it said: "
            << reported.toStdString();
    }

    QStringList left;
    for (const FileInfo &entry : target.provider->list(scratch.path, true))
        left << entry.name();
    EXPECT_EQ(left, QStringList{QStringLiteral("fixture.zip")})
        << "the download-copy path wrote back to the server";
}
