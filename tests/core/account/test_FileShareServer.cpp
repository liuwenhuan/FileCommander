#include <gtest/gtest.h>

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>

#include "account/FileShareServer.h"
#include "account/ShareIdentity.h"
#include "network/CurlWebDavProvider.h"

// The point of this file, and of the whole "serve WebDAV" decision: the
// accessing side is CurlWebDavProvider unchanged. So the assertions here are
// FileProvider contract assertions, made against a real socket -- if the server
// gets a verb, a status code or a PROPFIND body wrong, the provider fails the
// same way it would against a broken third-party server, and this test says so.
//
// Everything here now runs over TLS, because the server only speaks TLS. The
// pin is what makes that worth anything, so it gets tests of its own at the
// bottom of the file.
namespace {

const char kTicket[] = "ticket-for-the-tests";

QByteArray blob(int size, char seed) {
    QByteArray data(size, seed);
    for (int i = 0; i < size; ++i)
        data[i] = static_cast<char>((i * 31 + seed) & 0xff);
    return data;
}

bool writeFile(const QString &path, const QByteArray &data) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    return f.write(data) == data.size();
}

QByteArray readFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QByteArray();
    return f.readAll();
}

// One shared server plus one connected provider per test, torn down in order --
// the provider's streaming handles own sockets into the server, so the server
// must outlive them.
class FileShareServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(m_dir.isValid());
        m_share = m_dir.path() + QStringLiteral("/share");
        ASSERT_TRUE(QDir().mkpath(m_share + QStringLiteral("/sub")));
        ASSERT_TRUE(writeFile(m_share + QStringLiteral("/hello.txt"), "hello world"));
        // Something outside the share, to prove no path can reach it.
        ASSERT_TRUE(writeFile(m_dir.path() + QStringLiteral("/secret.txt"), "not yours"));

        m_server = new FileShareServer;
        m_server->setSharedFolders({m_share});
        m_server->addTicket(QString::fromLatin1(kTicket), 300);

        QSignalSpy up(m_server, &FileShareServer::started);
        QSignalSpy bad(m_server, &FileShareServer::failed);
        m_server->start();
        ASSERT_TRUE(up.wait(5000))
            << "server never bound a port"
            << (bad.isEmpty() ? std::string() : ": " + bad.first().first().toString().toStdString());
        m_port = up.first().first().toUInt();
        ASSERT_NE(m_port, 0u);
    }

    void TearDown() override {
        m_provider.reset();
        delete m_server;
        m_server = nullptr;
    }

    // Connects a provider with `password` as the ticket and `pin` as the peer
    // pin. Returns false when the server refused the credentials or the pin did
    // not match, which is what the 401 and wrong-pin tests assert.
    bool connectWith(const QString &password, const QString &pin = ShareIdentity::local().pin) {
        m_provider = std::make_shared<CurlWebDavProvider>();
        m_provider->setTimeoutMs(8000);
        m_provider->setPinnedPublicKey(pin);
        QString error;
        return m_provider->connectToHost(QStringLiteral("127.0.0.1"), int(m_port),
                                         QStringLiteral("device"), password,
                                         /*useHttps=*/true, &error);
    }

    void connectOk() { ASSERT_TRUE(connectWith(QString::fromLatin1(kTicket))); }

    QTemporaryDir m_dir;
    QString m_share;
    FileShareServer *m_server = nullptr;
    std::shared_ptr<CurlWebDavProvider> m_provider;
    quint16 m_port = 0;
};

TEST_F(FileShareServerTest, TheRootListsOneEntryPerSharedFolder) {
    connectOk();
    const QVector<FileInfo> root = m_provider->list(QStringLiteral("/"), true);
    ASSERT_EQ(root.size(), 1);
    EXPECT_EQ(root.first().name(), QStringLiteral("share"));
    EXPECT_TRUE(root.first().isDir());
}

TEST_F(FileShareServerTest, AShareListsItsContentsButNotItself) {
    connectOk();
    const QVector<FileInfo> entries = m_provider->list(QStringLiteral("/share"), true);
    QStringList names;
    for (const FileInfo &e : entries)
        names << e.name();
    names.sort();
    EXPECT_EQ(names, QStringList({QStringLiteral("hello.txt"), QStringLiteral("sub")}));
    for (const FileInfo &e : entries) {
        if (e.name() == QStringLiteral("hello.txt"))
            EXPECT_EQ(e.size(), 11);
    }
    EXPECT_TRUE(m_provider->isDir(QStringLiteral("/share/sub")));
    EXPECT_FALSE(m_provider->isDir(QStringLiteral("/share/hello.txt")));
    EXPECT_TRUE(m_provider->exists(QStringLiteral("/share/hello.txt")));
    EXPECT_FALSE(m_provider->exists(QStringLiteral("/share/nothing.txt")));
}

TEST_F(FileShareServerTest, AFileReadsBackByteForByte) {
    const QByteArray payload = blob(300 * 1024, 7);
    ASSERT_TRUE(writeFile(m_share + QStringLiteral("/big.bin"), payload));
    connectOk();

    FileHandle *h = m_provider->openRead(QStringLiteral("/share/big.bin"));
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(m_provider->handleSize(h), payload.size());
    QByteArray got;
    QByteArray chunk(64 * 1024, Qt::Uninitialized);
    while (true) {
        const qint64 n = m_provider->read(h, chunk.data(), chunk.size());
        ASSERT_GE(n, 0);
        if (n == 0)
            break;
        got.append(chunk.constData(), int(n));
    }
    EXPECT_TRUE(m_provider->closeHandleStatus(h));
    EXPECT_EQ(got, payload);
}

// Resumed downloads are the reason the server implements Range at all: an
// interrupted transfer must be able to ask for the tail only.
TEST_F(FileShareServerTest, ADownloadResumesFromAnOffset) {
    const QByteArray payload = blob(200 * 1024, 19);
    ASSERT_TRUE(writeFile(m_share + QStringLiteral("/resume.bin"), payload));
    connectOk();

    FileHandle *h = m_provider->openRead(QStringLiteral("/share/resume.bin"));
    ASSERT_NE(h, nullptr);
    const qint64 offset = 128 * 1024;
    ASSERT_TRUE(m_provider->seek(h, offset));
    QByteArray got;
    QByteArray chunk(32 * 1024, Qt::Uninitialized);
    while (true) {
        const qint64 n = m_provider->read(h, chunk.data(), chunk.size());
        ASSERT_GE(n, 0);
        if (n == 0)
            break;
        got.append(chunk.constData(), int(n));
    }
    EXPECT_TRUE(m_provider->closeHandleStatus(h));
    EXPECT_EQ(got, payload.mid(int(offset)));
}

TEST_F(FileShareServerTest, AnUploadLandsInTheSharedFolder) {
    const QByteArray payload = blob(150 * 1024, 3);
    connectOk();

    FileHandle *h = m_provider->openWrite(QStringLiteral("/share/up.bin"), true);
    ASSERT_NE(h, nullptr);
    m_provider->setExpectedWriteSize(h, payload.size());
    qint64 sent = 0;
    while (sent < payload.size()) {
        const qint64 n = m_provider->write(h, payload.constData() + sent,
                                           qMin<qint64>(48 * 1024, payload.size() - sent));
        ASSERT_GT(n, 0);
        sent += n;
    }
    EXPECT_TRUE(m_provider->closeHandleStatus(h));
    EXPECT_EQ(readFile(m_share + QStringLiteral("/up.bin")), payload);
}

// Without a declared size libcurl falls back to Transfer-Encoding: chunked, so
// the server has to accept both framings -- this is the one FileOperations
// actually uses when the source size is unknown.
TEST_F(FileShareServerTest, AnUploadOfUnknownSizeIsAcceptedChunked) {
    const QByteArray payload = blob(70 * 1024, 11);
    connectOk();

    FileHandle *h = m_provider->openWrite(QStringLiteral("/share/chunked.bin"), true);
    ASSERT_NE(h, nullptr);
    qint64 sent = 0;
    while (sent < payload.size()) {
        const qint64 n = m_provider->write(h, payload.constData() + sent,
                                           qMin<qint64>(16 * 1024, payload.size() - sent));
        ASSERT_GT(n, 0);
        sent += n;
    }
    EXPECT_TRUE(m_provider->closeHandleStatus(h));
    EXPECT_EQ(readFile(m_share + QStringLiteral("/chunked.bin")), payload);
}

TEST_F(FileShareServerTest, MkdirRenameAndDeleteAllWork) {
    connectOk();

    EXPECT_TRUE(m_provider->mkdir(QStringLiteral("/share/fresh")));
    EXPECT_TRUE(QFileInfo(m_share + QStringLiteral("/fresh")).isDir());

    QString newPath;
    EXPECT_EQ(m_provider->rename(QStringLiteral("/share/hello.txt"),
                                 QStringLiteral("renamed.txt"), &newPath),
              FileProvider::RenameResult::Ok);
    EXPECT_EQ(newPath, QStringLiteral("/share/renamed.txt"));
    EXPECT_FALSE(QFile::exists(m_share + QStringLiteral("/hello.txt")));
    EXPECT_EQ(readFile(m_share + QStringLiteral("/renamed.txt")), QByteArray("hello world"));

    EXPECT_EQ(m_provider->moveTo(QStringLiteral("/share/renamed.txt"),
                                 QStringLiteral("/share/sub/renamed.txt")),
              FileProvider::RenameResult::Ok);
    EXPECT_EQ(readFile(m_share + QStringLiteral("/sub/renamed.txt")),
              QByteArray("hello world"));

    EXPECT_TRUE(m_provider->remove(QStringLiteral("/share/sub/renamed.txt")));
    EXPECT_FALSE(QFile::exists(m_share + QStringLiteral("/sub/renamed.txt")));
    EXPECT_TRUE(m_provider->remove(QStringLiteral("/share/fresh")));
    EXPECT_FALSE(QFile::exists(m_share + QStringLiteral("/fresh")));
}

// FileProvider's rule for moveTo(): a non-Ok result must guarantee the source
// is untouched, because the caller then falls back to copy+delete. Moving onto
// an occupied path with the overwrite refused is the cheap way to provoke it.
TEST_F(FileShareServerTest, AFailedMoveLeavesTheSourceAlone) {
    ASSERT_TRUE(writeFile(m_share + QStringLiteral("/sub/hello.txt"), "already here"));
    connectOk();

    EXPECT_NE(m_provider->moveTo(QStringLiteral("/share/hello.txt"),
                                 QStringLiteral("/share/nope/hello.txt")),
              FileProvider::RenameResult::Ok);
    EXPECT_EQ(readFile(m_share + QStringLiteral("/hello.txt")), QByteArray("hello world"));
}

TEST_F(FileShareServerTest, NothingOutsideTheShareIsReachable) {
    connectOk();

    // Both spellings of the escape: the path as written, and the one a client
    // might send without normalising.
    for (const QString &path : {QStringLiteral("/share/../secret.txt"),
                                QStringLiteral("/share/sub/../../secret.txt")}) {
        EXPECT_FALSE(m_provider->exists(path)) << path.toStdString();
        FileHandle *h = m_provider->openRead(path);
        if (h) {
            char buf[64];
            EXPECT_LE(m_provider->read(h, buf, sizeof(buf)), 0) << path.toStdString();
            m_provider->closeHandle(h);
        }
    }
    // And the file itself is still there, i.e. nothing above deleted it.
    EXPECT_EQ(readFile(m_dir.path() + QStringLiteral("/secret.txt")), QByteArray("not yours"));
}

TEST_F(FileShareServerTest, AWrongTicketGetsNothing) {
    EXPECT_FALSE(connectWith(QStringLiteral("not-the-ticket")));
    // The connect failed, so the provider is disconnected; a listing attempt
    // must not somehow succeed anyway.
    EXPECT_TRUE(m_provider->list(QStringLiteral("/"), true).isEmpty());
}

TEST_F(FileShareServerTest, AnExpiredTicketStopsWorking) {
    m_server->addTicket(QStringLiteral("brief"), 0);
    EXPECT_FALSE(connectWith(QStringLiteral("brief")));
}

// The one that proves pinning is switched on rather than merely configured: a
// correct ticket, a reachable server, and nothing but a substituted pin between
// them. Without this test the whole feature could be decoration.
TEST_F(FileShareServerTest, AWrongPinGetsNothing) {
    const ShareIdentity::Identity other = ShareIdentity::generate();
    ASSERT_TRUE(other.isValid());
    ASSERT_NE(other.pin, ShareIdentity::local().pin);

    EXPECT_FALSE(connectWith(QString::fromLatin1(kTicket), other.pin));
    EXPECT_TRUE(m_provider->list(QStringLiteral("/"), true).isEmpty());

    // And the right pin still works against the same server, so the failure
    // above was the pin and not something else about this connection.
    EXPECT_TRUE(connectWith(QString::fromLatin1(kTicket)));
}

// The pin is only a shared secret if both sides derive it the same way. curl's
// documented recipe is the reference, so compare against it directly.
TEST(ShareIdentityTest, ThePinMatchesTheOpenSslRecipe) {
    const ShareIdentity::Identity identity = ShareIdentity::generate();
    ASSERT_TRUE(identity.isValid());
    EXPECT_TRUE(identity.pin.startsWith(QStringLiteral("sha256//")));
    EXPECT_EQ(ShareIdentity::pinForCertificate(identity.certPem), identity.pin);

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString certPath = dir.path() + QStringLiteral("/cert.pem");
    ASSERT_TRUE(writeFile(certPath, identity.certPem));

    // openssl x509 -pubkey -noout | openssl pkey -pubin -outform der
    //   | openssl dgst -sha256 -binary | base64
    QProcess openssl;
    openssl.start(QStringLiteral("sh"),
                  {QStringLiteral("-c"),
                   QStringLiteral("openssl x509 -in %1 -pubkey -noout | "
                                  "openssl pkey -pubin -outform der | "
                                  "openssl dgst -sha256 -binary | base64 -w0")
                       .arg(certPath)});
    if (!openssl.waitForFinished(15000) || openssl.exitStatus() != QProcess::NormalExit ||
        openssl.exitCode() != 0)
        GTEST_SKIP() << "no usable openssl CLI on this machine";
    const QString expected =
        QStringLiteral("sha256//") + QString::fromLatin1(openssl.readAllStandardOutput()).trimmed();
    EXPECT_EQ(identity.pin, expected);
}

} // namespace
