#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QProcess>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QThread>

#include "CurlWebDavProvider.h"
#include "FileOperations.h"
#include "LocalFileProvider.h"

// Regression cover for a silent data-loss bug in the WebDAV upload path.
//
// The provider streams a PUT body through a read callback that cannot rewind.
// When the server answers the first (unauthenticated) request with a 401
// challenge, curl retries with credentials -- but the first attempt has already
// drained the pipe, so the retry sends nothing. The server stores an EMPTY file
// and answers 201 Created, and every layer above reports success. A 16-byte
// write landed as 0 bytes against a real server.
//
// The failure needs a server that actually issues the 401 challenge, so these
// tests drive a real wsgidav instance. Where it isn't installed the tests skip
// rather than pass vacuously -- a test that cannot observe the bug is worse than
// no test, because it reads like cover that isn't there.
namespace {

quint16 availablePort() {
    QTcpServer probe;
    if (!probe.listen(QHostAddress::LocalHost, 0))
        return 0;
    return probe.serverPort();
}

bool haveWsgidav() {
    QProcess p;
    p.start("python3", {"-c", "import wsgidav, cheroot"});
    return p.waitForFinished(10000) && p.exitCode() == 0;
}

// Runs a password-protected wsgidav over `root` for the life of the fixture.
class DavServer {
public:
    explicit DavServer(const QString &root)
        : m_port(availablePort()) {
        if (m_port == 0)
            return;
        const QString script = QStringLiteral(
            "from wsgidav.wsgidav_app import WsgiDAVApp\n"
            "from cheroot import wsgi\n"
            "app = WsgiDAVApp({'provider_mapping': {'/': '%1'},\n"
            "  'simple_dc': {'user_mapping': {'*': {'u': {'password': 'p'}}}},\n"
            "  'verbose': 0, 'logging': {'enable': False}})\n"
            "wsgi.Server(('127.0.0.1', %2), app).start()\n").arg(root).arg(m_port);
        m_proc.start("python3", {"-c", script});
        m_started = m_proc.waitForStarted(10000);
        // Wait for the port to answer rather than sleeping a fixed amount.
        for (int i = 0; m_started && i < 100 && !reachable(); ++i)
            QThread::msleep(100);
    }

    ~DavServer() {
        if (m_started) {
            m_proc.kill();
            m_proc.waitForFinished(5000);
        }
    }

    quint16 port() const { return m_port; }

    bool reachable() const {
        if (!m_started)
            return false;
        QProcess c;
        c.start("curl", {"-s", "-o", "/dev/null", "-m", "2",
                         QStringLiteral("http://127.0.0.1:%1/").arg(m_port)});
        return c.waitForFinished(5000) && c.exitCode() == 0;
    }

private:
    quint16 m_port = 0;
    bool m_started = false;
    QProcess m_proc;
};

// Byte at each position differs, so a truncated or duplicated body is caught.
QByteArray patterned(int size) {
    QByteArray d;
    d.resize(size);
    for (int i = 0; i < size; ++i)
        d[i] = static_cast<char>(i % 251);
    return d;
}

bool upload(CurlWebDavProvider &dav, const QString &path, const QByteArray &data) {
    FileHandle *h = dav.openWrite(path, true);
    if (!h)
        return false;
    if (!data.isEmpty() && dav.write(h, data.constData(), data.size()) != data.size()) {
        dav.closeHandle(h);
        return false;
    }
    return dav.closeHandleStatus(h);
}

} // namespace

// The headline contract: what the provider says it wrote must be what the
// server actually holds. Asserting on the local file (not just the HTTP status)
// is the whole point -- the bug returned 201 Created for an empty file.
TEST(WebDavUploadTest, UploadedBytesActuallyLandOnTheServer) {
    if (!haveWsgidav())
        GTEST_SKIP() << "wsgidav/cheroot not installed; cannot serve a 401 challenge";
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    DavServer server(root.path());
    if (!server.reachable())
        GTEST_SKIP() << "local WebDAV server did not start";

    CurlWebDavProvider dav;
    dav.setTimeoutMs(15000);
    QString err;
    ASSERT_TRUE(dav.connectToHost("127.0.0.1", server.port(), "u", "p", false, &err))
        << err.toStdString();

    const QByteArray payload = patterned(4096);
    ASSERT_TRUE(upload(dav, "/landed.bin", payload));

    const QString onDisk = QDir(root.path()).filePath("landed.bin");
    ASSERT_TRUE(QFile::exists(onDisk)) << "server kept no file at all";
    QFile f(onDisk);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray stored = f.readAll();
    // Before the fix this was 0 bytes while the upload reported success.
    EXPECT_EQ(stored.size(), payload.size());
    EXPECT_EQ(stored, payload);
}

// The same guarantee read back through the provider's own download path, so a
// round trip is verified end to end rather than only against the filesystem.
TEST(WebDavUploadTest, CopyAcrossProvidersWritesTheCompleteServerSideFile) {
    if (!haveWsgidav())
        GTEST_SKIP() << "wsgidav/cheroot not installed; cannot serve a 401 challenge";
    QTemporaryDir serverRoot;
    QTemporaryDir sourceRoot;
    ASSERT_TRUE(serverRoot.isValid() && sourceRoot.isValid());
    DavServer server(serverRoot.path());
    if (!server.reachable())
        GTEST_SKIP() << "local WebDAV server did not start";

    const QByteArray payload = patterned(8192);
    const QString source = QDir(sourceRoot.path()).filePath("cross-provider.bin");
    QFile sourceFile(source);
    ASSERT_TRUE(sourceFile.open(QIODevice::WriteOnly));
    ASSERT_EQ(sourceFile.write(payload), payload.size());
    sourceFile.close();

    CurlWebDavProvider dav;
    dav.setTimeoutMs(15000);
    QString err;
    ASSERT_TRUE(dav.connectToHost("127.0.0.1", server.port(), "u", "p", false, &err))
        << err.toStdString();

    FileOperations operations;
    ASSERT_TRUE(operations.copyAcrossProviders(LocalFileProvider::instance(), {source}, &dav, "/",
                                               /*removeSource=*/false, nullptr, &err))
        << err.toStdString();

    QFile stored(QDir(serverRoot.path()).filePath("cross-provider.bin"));
    ASSERT_TRUE(stored.open(QIODevice::ReadOnly));
    EXPECT_EQ(stored.size(), payload.size());
    EXPECT_EQ(stored.readAll(), payload);
}

TEST(WebDavUploadTest, RoundTripsThroughTheProvider) {
    if (!haveWsgidav())
        GTEST_SKIP() << "wsgidav/cheroot not installed; cannot serve a 401 challenge";
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    DavServer server(root.path());
    if (!server.reachable())
        GTEST_SKIP() << "local WebDAV server did not start";

    CurlWebDavProvider dav;
    dav.setTimeoutMs(15000);
    QString err;
    ASSERT_TRUE(dav.connectToHost("127.0.0.1", server.port(), "u", "p", false, &err))
        << err.toStdString();

    const QByteArray payload = patterned(2048);
    ASSERT_TRUE(upload(dav, "/roundtrip.bin", payload));

    FileHandle *r = dav.openRead("/roundtrip.bin");
    ASSERT_NE(r, nullptr);
    QByteArray got;
    char buf[4096];
    qint64 n;
    while ((n = dav.read(r, buf, sizeof buf)) > 0)
        got.append(buf, static_cast<int>(n));
    dav.closeHandle(r);

    EXPECT_EQ(got.size(), payload.size());
    EXPECT_EQ(got, payload);
}

// A name needing percent-encoding must survive the same path: the encoding runs
// per segment, and an upload is where a mis-encoded URL would silently create
// the wrong file rather than fail loudly.
TEST(WebDavUploadTest, NonAsciiNameUploadsIntact) {
    if (!haveWsgidav())
        GTEST_SKIP() << "wsgidav/cheroot not installed; cannot serve a 401 challenge";
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    DavServer server(root.path());
    if (!server.reachable())
        GTEST_SKIP() << "local WebDAV server did not start";

    CurlWebDavProvider dav;
    dav.setTimeoutMs(15000);
    QString err;
    ASSERT_TRUE(dav.connectToHost("127.0.0.1", server.port(), "u", "p", false, &err))
        << err.toStdString();

    const QString name = QString::fromUtf8("\xe4\xb8\xad\xe6\x96\x87 \xe6\x96\x87\xe4\xbb\xb6.bin");
    const QByteArray payload = patterned(1024);
    ASSERT_TRUE(upload(dav, "/" + name, payload));

    QFile f(QDir(root.path()).filePath(name));
    ASSERT_TRUE(f.exists()) << "file did not land under the expected name";
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    EXPECT_EQ(f.readAll(), payload);
}
