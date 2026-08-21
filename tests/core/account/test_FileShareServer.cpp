#include <gtest/gtest.h>

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include "LocalFileProvider.h"
#include "OperationQueue.h"
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

// The server writes an aborted PUT's partial body on its own thread and only
// closes the file when the dropped connection is reaped, so what landed is not
// readable the instant closeHandleStatus() returns. Wait for the size to stop
// moving.
qint64 settledSize(const QString &path) {
    qint64 last = -1;
    int stable = 0;
    for (int i = 0; i < 100; ++i) {
        QThread::msleep(20);
        const qint64 now = QFileInfo(path).size();
        stable = (now == last && now > 0) ? stable + 1 : 0;
        if (stable >= 2)
            return now;
        last = now;
    }
    return last;
}

// Sends payload.mid(offset) exactly the way FileOperations::streamCopy() drives
// a resumed transfer: openWrite(truncate=false), declare the *tail* length (not
// the file length), seek the write handle to the resume point, then stream.
// Returns closeHandleStatus(), i.e. whether the server committed it.
bool putFrom(CurlWebDavProvider *provider, const QString &davPath, const QByteArray &payload,
             qint64 offset) {
    FileHandle *h = provider->openWrite(davPath, offset == 0);
    if (!h)
        return false;
    provider->setExpectedWriteSize(h, payload.size() - offset);
    if (!provider->seek(h, offset)) {
        provider->closeHandle(h);
        return false;
    }
    qint64 sent = offset;
    while (sent < payload.size()) {
        const qint64 n = provider->write(h, payload.constData() + sent,
                                         qMin<qint64>(64 * 1024, payload.size() - sent));
        if (n <= 0)
            break;
        sent += n;
    }
    return provider->closeHandleStatus(h);
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

        m_server = new FileShareServer(nullptr);
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

TEST_F(FileShareServerTest, AnUploadSignalsThatAFileWasReceived) {
    const QByteArray payload = blob(48 * 1024, 5);
    connectOk();

    QSignalSpy received(m_server, &FileShareServer::received);

    FileHandle *h = m_provider->openWrite(QStringLiteral("/share/arrived.bin"), true);
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

    // The signal travels from the server's worker thread back to this thread as
    // a queued connection, so it may land a moment after closeHandleStatus().
    if (received.isEmpty())
        received.wait(5000);
    ASSERT_EQ(received.count(), 1);
    // resolve() reports the canonical path, so compare against that, not the
    // raw share + name string.
    EXPECT_EQ(received.first().first().toString(),
              QFileInfo(m_share + QStringLiteral("/arrived.bin")).canonicalFilePath());
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

// --- Resume ------------------------------------------------------------------
//
// Device-to-device transfer is the one WebDAV link where an interrupted upload
// can be continued: a PUT normally replaces the whole resource, so the provider
// only ever resumes against a server that promised, at connect time, to honour
// Content-Range on a PUT. These tests are the proof that promise is kept.

// The strong form of the claim: what is already on the server before the resume
// point must still be there afterwards. The destination is pre-filled with
// bytes that are deliberately NOT the source's, so a server that quietly
// restarted the PUT from zero, or truncated the file when it opened it, fails
// here even though the transfer itself reports success.
TEST_F(FileShareServerTest, AResumedUploadKeepsWhatIsAlreadyOnTheServer) {
    connectOk();
    // The gate FileOperations::transferFile() consults before resuming at all.
    EXPECT_TRUE(m_provider->supportsWriteResume());

    const QByteArray payload = blob(3 * 1024 * 1024, 23);
    const int offset = 1024 * 1024;
    const QByteArray sentinel(offset, '\x5a');
    const QString local = m_share + QStringLiteral("/sentinel.bin");
    ASSERT_TRUE(writeFile(local, sentinel));

    EXPECT_TRUE(putFrom(m_provider.get(), QStringLiteral("/share/sentinel.bin"), payload, offset));

    const QByteArray got = readFile(local);
    ASSERT_EQ(got.size(), payload.size());
    EXPECT_EQ(got.left(offset), sentinel) << "the server rewrote bytes before the resume point";
    EXPECT_EQ(got.mid(offset), payload.mid(offset));
}

// The realistic round trip: a multi-megabyte upload dies partway, and the
// second attempt sends only the tail. The partial file the server is left
// holding is what transferFile() measures to pick the offset, so that size is
// asserted too -- it is the input to the whole decision.
TEST_F(FileShareServerTest, AnInterruptedUploadContinuesWhereItStopped) {
    connectOk();
    const QByteArray payload = blob(3 * 1024 * 1024, 5);
    const QString dav = QStringLiteral("/share/interrupted.bin");
    const QString local = m_share + QStringLiteral("/interrupted.bin");

    // First attempt: declare the whole file, then hang up a third of the way in.
    FileHandle *h = m_provider->openWrite(dav, true);
    ASSERT_NE(h, nullptr);
    m_provider->setExpectedWriteSize(h, payload.size());
    qint64 sent = 0;
    while (sent < 1024 * 1024) {
        const qint64 n = m_provider->write(h, payload.constData() + sent,
                                           qMin<qint64>(64 * 1024, payload.size() - sent));
        ASSERT_GT(n, 0);
        sent += n;
    }
    // The body is short of the declared Content-Length, so this must not be
    // reported as a completed upload.
    EXPECT_FALSE(m_provider->closeHandleStatus(h));

    const qint64 partial = settledSize(local);
    ASSERT_GT(partial, 0) << "nothing survived the interrupted upload, so there is nothing to resume";
    ASSERT_LT(partial, payload.size());

    // The size the resume decision is made on, read back through the provider.
    FileHandle *probe = m_provider->openRead(dav);
    ASSERT_NE(probe, nullptr);
    EXPECT_EQ(m_provider->handleSize(probe), partial);
    m_provider->closeHandle(probe);

    EXPECT_TRUE(putFrom(m_provider.get(), dav, payload, partial));
    EXPECT_EQ(readFile(local), payload);
}

// A resume offset the server cannot honour (it holds fewer bytes than that)
// must be refused outright rather than written at the wrong place: the caller
// falls back to a fresh copy, and the file on disk is left as it was.
TEST_F(FileShareServerTest, AResumeBeyondWhatTheServerHoldsIsRefused) {
    connectOk();
    const QByteArray payload = blob(256 * 1024, 41);
    const QByteArray already = blob(4096, 77);
    const QString local = m_share + QStringLiteral("/short.bin");
    ASSERT_TRUE(writeFile(local, already));

    // Not putFrom(): the refusal has to come from the server, so the client
    // side of the handshake is asserted to have succeeded first.
    FileHandle *h = m_provider->openWrite(QStringLiteral("/share/short.bin"), false);
    ASSERT_NE(h, nullptr);
    m_provider->setExpectedWriteSize(h, payload.size() - 128 * 1024);
    ASSERT_TRUE(m_provider->seek(h, 128 * 1024));
    qint64 sent = 128 * 1024;
    while (sent < payload.size()) {
        const qint64 n = m_provider->write(h, payload.constData() + sent,
                                           qMin<qint64>(64 * 1024, payload.size() - sent));
        if (n <= 0)
            break;
        sent += n;
    }
    EXPECT_FALSE(m_provider->closeHandleStatus(h));
    EXPECT_EQ(readFile(local), already);
}

// Download resume, driven the way streamCopy() drives it: the first attempt
// dies partway, the second asks for the tail only. If the server answered 200
// from byte zero instead of 206 from the offset, the reassembled file would be
// the wrong length and the wrong bytes -- both are asserted.
TEST_F(FileShareServerTest, AnInterruptedDownloadContinuesWhereItStopped) {
    const QByteArray payload = blob(3 * 1024 * 1024, 31);
    ASSERT_TRUE(writeFile(m_share + QStringLiteral("/down.bin"), payload));
    connectOk();
    const QString dav = QStringLiteral("/share/down.bin");

    // First attempt: take a megabyte, then drop the connection.
    QByteArray got;
    FileHandle *h = m_provider->openRead(dav);
    ASSERT_NE(h, nullptr);
    QByteArray chunk(64 * 1024, Qt::Uninitialized);
    while (got.size() < 1024 * 1024) {
        const qint64 n = m_provider->read(h, chunk.data(), chunk.size());
        ASSERT_GT(n, 0);
        got.append(chunk.constData(), int(n));
    }
    m_provider->closeHandle(h);
    const int offset = got.size();
    ASSERT_LT(offset, payload.size());

    // Second attempt: the tail only.
    h = m_provider->openRead(dav);
    ASSERT_NE(h, nullptr);
    ASSERT_TRUE(m_provider->seek(h, offset));
    QByteArray tail;
    while (true) {
        const qint64 n = m_provider->read(h, chunk.data(), chunk.size());
        ASSERT_GE(n, 0);
        if (n == 0)
            break;
        tail.append(chunk.constData(), int(n));
    }
    EXPECT_TRUE(m_provider->closeHandleStatus(h));
    EXPECT_EQ(tail.size(), payload.size() - offset) << "the server resent the whole file";
    EXPECT_EQ(got + tail, payload);
}

// Everything above drives the provider by hand. Production does not: it hands
// the job to OperationQueue, and the decision to resume at all is taken by
// FileOperations::transferFile, which asks the destination provider whether it
// supports write resume and how much of the file it already holds. That
// decision has never run against this provider, and if it fell through to
// conflict resolution instead, none of the resume work above would ever be
// reached.
TEST_F(FileShareServerTest, TheTransferQueueResumesAnInterruptedCopy) {
    const QByteArray payload = blob(16 * 1024 * 1024, 61);
    const QString source = m_dir.path() + QStringLiteral("/queued.bin");
    const QString landed = m_share + QStringLiteral("/queued.bin");
    ASSERT_TRUE(writeFile(source, payload));
    connectOk();

    auto local = std::make_shared<LocalFileProvider>();
    OperationQueue queue;
    queue.setMaxConcurrentTransfers(1);
    // A prompt here would mean the transfer took the conflict branch instead of
    // resuming, so record it rather than answering it.
    bool conflictAsked = false;
    queue.setConflictHandler([&conflictAsked](const FileConflict &) {
        conflictAsked = true;
        return ErrorAction::Skip;
    });

    // First attempt, stopped mid-file: cancelActiveJobs() is honoured between
    // chunks, so the server keeps whatever had already arrived.
    qint64 firstBytesSeen = 0;
    bool stopped = false;
    QObject::connect(&queue, &OperationQueue::progress, &queue,
                     [&](qint64, qint64, qint64 doneBytes, qint64, const QString &) {
                         if (firstBytesSeen == 0 && doneBytes > 0)
                             firstBytesSeen = doneBytes;
                         if (!stopped && doneBytes >= 4 * 1024 * 1024) {
                             stopped = true;
                             queue.cancelActiveJobs();
                         }
                     });

    QSignalSpy interrupted(&queue, &OperationQueue::finished);
    queue.enqueueProviderCopy(local, {source}, m_provider, QStringLiteral("/share"));
    ASSERT_TRUE(interrupted.count() > 0 || interrupted.wait(60000));
    EXPECT_FALSE(interrupted.takeFirst().at(0).toBool());
    ASSERT_TRUE(stopped) << "the copy finished before it could be interrupted";

    const qint64 partial = settledSize(landed);
    ASSERT_GT(partial, 128 * 1024) << "too little landed to tell a resume from a restart";
    ASSERT_LT(partial, payload.size());

    // Second attempt, through the same call the UI makes. transferFile sees a
    // shorter file at the destination and resumes it.
    firstBytesSeen = 0;
    QSignalSpy done(&queue, &OperationQueue::finished);
    queue.enqueueProviderCopy(local, {source}, m_provider, QStringLiteral("/share"));
    ASSERT_TRUE(done.count() > 0 || done.wait(60000));
    EXPECT_TRUE(done.takeFirst().at(0).toBool());

    EXPECT_FALSE(conflictAsked) << "the transfer asked to overwrite instead of resuming";
    // The resume credits the bytes already on the server before sending one of
    // its own, so the first progress of the second attempt is the resume point.
    // A restart from zero would report its first chunk, 64 KiB, instead.
    EXPECT_GE(firstBytesSeen, partial) << "the second attempt re-sent the prefix";
    EXPECT_EQ(readFile(landed), payload);
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
