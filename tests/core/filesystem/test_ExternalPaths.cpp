#include <gtest/gtest.h>

#include <QMimeData>
#include <QUrl>

#include "ExternalPaths.h"
#include "FileProvider.h"
#include "LocalFileProvider.h"

// What FileCommander tells *another application* when files are copied to the
// clipboard or dragged out of a panel.
//
// The bug this pins down: a panel's path is its backend's path, so a share's
// "/share/report.pdf" was handed out as file:///share/report.pdf. Dropped into
// another file manager that either finds nothing or -- the dangerous case --
// silently opens a same-named LOCAL file. The same held for an archive tab,
// whose "/etc/passwd" entry named the real /etc/passwd on the way out.
namespace {

// A backend with a server behind it, but no live gvfs mount (which is the state
// the tests run in: `loc.host` is a name nothing on this machine has mounted).
class FakeRemote : public FileProvider {
public:
    explicit FakeRemote(QString scheme, QString host, QString user)
        : m_scheme(std::move(scheme)), m_host(std::move(host)), m_user(std::move(user)) {}

    QString displayName() const override { return m_user + QLatin1Char('@') + m_host; }
    QString scheme() const override { return m_scheme; }
    RemoteLocation remoteLocation() const override {
        RemoteLocation loc;
        loc.scheme = m_scheme;
        loc.host = m_host;
        loc.user = m_user;
        return loc;
    }

    QVector<FileInfo> list(const QString &, bool) const override { return {}; }
    bool isDir(const QString &) const override { return true; }
    QString cleanPath(const QString &p) const override { return p; }
    QString parentPath(const QString &) const override { return {}; }
    bool exists(const QString &) const override { return true; }
    RenameResult rename(const QString &, const QString &, QString *) override {
        return RenameResult::Failed;
    }

private:
    QString m_scheme, m_host, m_user;
};

// An archive backend: not local, and with no server to name either. Its paths
// name entries inside an archive file and mean nothing outside this process.
class FakeArchive : public FileProvider {
public:
    QVector<FileInfo> list(const QString &, bool) const override { return {}; }
    bool isDir(const QString &) const override { return true; }
    QString cleanPath(const QString &p) const override { return p; }
    QString parentPath(const QString &) const override { return {}; }
    bool exists(const QString &) const override { return true; }
    RenameResult rename(const QString &, const QString &, QString *) override {
        return RenameResult::Failed;
    }
};

RemoteLocation smbLocation() {
    RemoteLocation loc;
    loc.scheme = QStringLiteral("smb");
    loc.host = QStringLiteral("nas.invalid");
    loc.user = QStringLiteral("deepin");
    return loc;
}

} // namespace

// --- the policy, in isolation ------------------------------------------------

TEST(ExternalPathsTest, LocalPathIsHandedOutUnchanged) {
    const QUrl url = fc::externalUrlFor(/*localFilesystem=*/true, RemoteLocation(),
                                        QStringLiteral("/home/deepin/report.pdf"), QString());
    EXPECT_TRUE(url.isLocalFile());
    EXPECT_EQ(url.toLocalFile().toStdString(), "/home/deepin/report.pdf");
}

TEST(ExternalPathsTest, MountedRemoteFileIsHandedOutAsItsRealLocalPath) {
    // The connection is mounted, so the file has a genuine name on this machine
    // that any program can open -- straight off the server, nothing copied.
    const QString mounted =
        QStringLiteral("/run/user/1000/gvfs/smb-share:server=nas,share=docs/report.pdf");
    const QUrl url = fc::externalUrlFor(/*localFilesystem=*/false, smbLocation(),
                                        QStringLiteral("/docs/report.pdf"), mounted);
    EXPECT_TRUE(url.isLocalFile());
    EXPECT_EQ(url.toLocalFile().toStdString(), mounted.toStdString());
}

TEST(ExternalPathsTest, UnmountedRemoteFileFallsBackToItsProtocolUri) {
    // No mount: say what the file actually is. A GIO/KIO-based program resolves
    // smb:// itself; anything else refuses the drop, which is the point --
    // refusing is not the failure mode, opening the wrong file is.
    const QUrl url = fc::externalUrlFor(/*localFilesystem=*/false, smbLocation(),
                                        QStringLiteral("/docs/report.pdf"), QString());
    EXPECT_FALSE(url.isLocalFile()) << "handed out a local path for a file on a server";
    EXPECT_EQ(url.scheme().toStdString(), "smb");
    EXPECT_EQ(url.host().toStdString(), "nas.invalid");
    EXPECT_EQ(url.path().toStdString(), "/docs/report.pdf");
}

TEST(ExternalPathsTest, RemotePathIsNeverHandedOutAsALocalFileUrl) {
    // The regression itself: /home exists on this machine AND on the share, so
    // a file:// URL over the server's path resolves to a local file of the same
    // name and the receiving application silently gets the wrong bytes.
    const QUrl url = fc::externalUrlFor(/*localFilesystem=*/false, smbLocation(),
                                        QStringLiteral("/home/deepin/.bashrc"), QString());
    ASSERT_FALSE(url.isEmpty());
    EXPECT_NE(url.toString().toStdString(), "file:///home/deepin/.bashrc");
    EXPECT_FALSE(url.isLocalFile());
}

TEST(ExternalPathsTest, ArchiveEntryIsHandedOutAsNothingAtAll) {
    // An archive holding "etc/passwd" browses as the path "/etc/passwd". There
    // is no URI form for an entry inside an archive, so the only honest answer
    // is silence -- never file:///etc/passwd.
    const QUrl url = fc::externalUrlFor(/*localFilesystem=*/false, RemoteLocation(),
                                        QStringLiteral("/etc/passwd"), QString());
    EXPECT_TRUE(url.isEmpty()) << "offered " << url.toString().toStdString();
}

// --- the same, driven through a provider -------------------------------------

TEST(ExternalPathsTest, LocalProviderStillYieldsPlainFileUrls) {
    // The local case must be untouched: this is what every other file manager,
    // editor and terminal receives today.
    const QStringList paths = {QStringLiteral("/tmp/a.txt"), QStringLiteral("/tmp/b.txt")};
    const QList<QUrl> urls = fc::externalUrlsFor(LocalFileProvider::instance(), paths);
    ASSERT_EQ(urls.size(), 2);
    EXPECT_EQ(urls.at(0).toString().toStdString(), "file:///tmp/a.txt");
    EXPECT_EQ(urls.at(1).toString().toStdString(), "file:///tmp/b.txt");
}

TEST(ExternalPathsTest, RemoteProviderYieldsUrisNotLocalPaths) {
    FakeRemote remote(QStringLiteral("smb"), QStringLiteral("nas.invalid"),
                      QStringLiteral("deepin"));
    const QStringList paths = {QStringLiteral("/docs/a.txt"), QStringLiteral("/docs/b.txt")};
    const QList<QUrl> urls = fc::externalUrlsFor(&remote, paths);
    ASSERT_EQ(urls.size(), 2);
    for (const QUrl &url : urls)
        EXPECT_FALSE(url.isLocalFile()) << url.toString().toStdString();
}

TEST(ExternalPathsTest, ArchiveProviderYieldsNothing) {
    FakeArchive archive;
    const QList<QUrl> urls =
        fc::externalUrlsFor(&archive, {QStringLiteral("/etc/passwd")});
    EXPECT_TRUE(urls.isEmpty());
}

TEST(ExternalPathsTest, NonAsciiRemoteNameSurvivesAsAnEncodedUri) {
    FakeRemote remote(QStringLiteral("smb"), QStringLiteral("nas.invalid"),
                      QStringLiteral("deepin"));
    const QString path = QStringLiteral("/video/3d动画 片/第一集.mp4");
    const QList<QUrl> urls = fc::externalUrlsFor(&remote, {path});
    ASSERT_EQ(urls.size(), 1);
    // Percent-encoded on the wire, but decoding it must give the path back
    // exactly -- a mangled name is a file the receiver cannot open either.
    EXPECT_EQ(urls.at(0).path().toStdString(), path.toStdString());
}

// --- the private channel that keeps in-app paste/drop working -----------------

TEST(ExternalPathsTest, InternalPathsRoundTrip) {
    const QStringList paths = {QStringLiteral("/docs/a.txt"),
                               QStringLiteral("/docs/名字 带空格.txt")};
    bool cut = false;
    EXPECT_EQ(fc::decodeInternalPaths(fc::encodeInternalPaths(paths, true), &cut), paths);
    EXPECT_TRUE(cut);
    EXPECT_EQ(fc::decodeInternalPaths(fc::encodeInternalPaths(paths, false), &cut), paths);
    EXPECT_FALSE(cut);
}

TEST(ExternalPathsTest, PayloadCarriesRemotePathsPrivatelyEvenWithNoPublicUrls) {
    FakeArchive archive;
    QMimeData mime;
    const QStringList paths = {QStringLiteral("/etc/passwd")};
    fc::setPathPayload(&mime, &archive, paths, /*cut=*/false);

    // Nothing for the outside world ...
    EXPECT_FALSE(mime.hasUrls()) << "an archive entry was advertised to other applications";
    // ... but our own drop/paste still gets the exact entry paths.
    EXPECT_EQ(fc::incomingPaths(&mime), paths);
}

TEST(ExternalPathsTest, IncomingPathsPrefersThePrivateFormat) {
    // A drag from a network panel carries both: smb:// URIs for the outside
    // world and the server's own paths for us. Reading the URIs instead would
    // hand the operation a URL string where a path belongs.
    FakeRemote remote(QStringLiteral("smb"), QStringLiteral("nas.invalid"),
                      QStringLiteral("deepin"));
    QMimeData mime;
    const QStringList paths = {QStringLiteral("/docs/report.pdf")};
    fc::setPathPayload(&mime, &remote, paths, /*cut=*/false);

    EXPECT_EQ(fc::incomingPaths(&mime), paths);
    EXPECT_TRUE(fc::hasIncomingPaths(&mime));
}

TEST(ExternalPathsTest, IncomingPathsStillReadsAnotherApplicationsFileUrls) {
    // Dropping from Nautilus: no private format, just file:// URLs.
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(QStringLiteral("/tmp/from-nautilus.txt")),
                  QUrl(QStringLiteral("smb://elsewhere.invalid/share/x.txt"))});
    // The remote URI is dropped: this process has no connection to that server,
    // so there is nothing it could copy from.
    const QStringList paths = fc::incomingPaths(&mime);
    ASSERT_EQ(paths.size(), 1);
    EXPECT_EQ(paths.first().toStdString(), "/tmp/from-nautilus.txt");
    EXPECT_TRUE(fc::hasIncomingPaths(&mime));
}

TEST(ExternalPathsTest, EmptyMimeCarriesNothing) {
    QMimeData mime;
    EXPECT_TRUE(fc::incomingPaths(&mime).isEmpty());
    EXPECT_FALSE(fc::hasIncomingPaths(&mime));
    EXPECT_FALSE(fc::hasIncomingPaths(nullptr));
}
