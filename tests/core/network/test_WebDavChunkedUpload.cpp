#include <gtest/gtest.h>

#include <QByteArray>
#include <QElapsedTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>

#include "CurlWebDavProvider.h"
#include "diagnostics/RuntimeCounters.h"

// Regression cover for a second silent data-loss bug on the WebDAV upload path,
// distinct from the 401-rewind one in test_WebDavUpload.cpp.
//
// The provider streamed every PUT with Transfer-Encoding: chunked, because the
// streaming API has no way to learn the total size up front. Two things went
// wrong with that, and both were measured against a real NAS reached through a
// local HTTP proxy:
//
//   1. The proxy refused chunked request bodies above ~16 KB outright: it
//      answered "100 Continue", swallowed the body, then closed without ever
//      sending a final response. 64 KB, 300 KB and 1.5 MB uploads all stored a
//      0-byte file. Sending Content-Length instead succeeded at every size up
//      to 8 MB against the same proxy and server.
//
//   2. Worse, the provider judged success on the CURLcode alone. curl reports
//      CURLE_OK for "100 Continue then clean close" -- there was no transport
//      error -- so closeHandleStatus() returned true for uploads that stored
//      nothing. PROPFIND read back getcontentlength 0 while the app reported
//      the copy complete.
//
// These tests drive a purpose-built TCP server instead of a real WebDAV
// implementation: the bug lives in how the provider frames the request and
// judges the response, so what is needed is a peer that reproduces the
// misbehaving framing exactly. A conforming server (wsgidav) cannot show it.
namespace {

// A minimal HTTP peer that runs on its own thread, captures the first request's
// headers, and replies with a caller-chosen script.
class ScriptedHttpServer : public QObject {
public:
    enum class Reply {
        Created,           // a well-behaved 201, the success path
        ContinueThenClose, // "100 Continue" then hang up: the proxy's misbehaviour
        ServiceUnavailable,
        Unauthorized,
        Forbidden,
        ProxyAuthenticationRequired,
        InsufficientStorage
    };

    explicit ScriptedHttpServer(Reply reply) : m_reply(reply) {
        m_server.listen(QHostAddress::LocalHost, 0);
    }

    quint16 port() const { return m_server.serverPort(); }

    // Answers connectToHost()'s opening PROPFIND with a minimal 207 so the
    // provider reaches the connected state. The handshake is not what these
    // tests are about -- the PUT that follows is.
    bool serveHandshake(int timeoutMs = 15000) {
        if (!m_server.waitForNewConnection(timeoutMs))
            return false;
        QTcpSocket *sock = m_server.nextPendingConnection();
        if (!sock)
            return false;

        QByteArray head;
        while (!head.contains("\r\n\r\n") && sock->waitForReadyRead(timeoutMs))
            head += sock->readAll();

        static const char kBody[] =
            "<?xml version=\"1.0\"?><D:multistatus xmlns:D=\"DAV:\"><D:response>"
            "<D:href>/</D:href><D:propstat><D:prop><D:resourcetype>"
            "<D:collection/></D:resourcetype></D:prop>"
            "<D:status>HTTP/1.1 200 OK</D:status></D:propstat>"
            "</D:response></D:multistatus>";
        const QByteArray resp = "HTTP/1.1 207 Multi-Status\r\nContent-Type: application/xml\r\n"
                                "Content-Length: " + QByteArray::number(int(sizeof(kBody) - 1)) +
                                "\r\n\r\n" + QByteArray(kBody);
        sock->write(resp);
        sock->flush();
        sock->waitForBytesWritten(timeoutMs);
        sock->disconnectFromHost();
        sock->deleteLater();
        return true;
    }

    // Serves exactly one request, blocking until it is done. Returns the request
    // headers (everything up to the blank line).
    QByteArray serveOnce(int timeoutMs = 15000) {
        if (!m_server.waitForNewConnection(timeoutMs))
            return {};
        QTcpSocket *sock = m_server.nextPendingConnection();
        if (!sock)
            return {};

        QByteArray head;
        while (!head.contains("\r\n\r\n") && sock->waitForReadyRead(timeoutMs))
            head += sock->readAll();

        // curl waits for a 100-continue before sending the body, so answer it
        // first in both scripts -- otherwise the success case stalls until
        // curl's internal timeout rather than uploading promptly.
        if (head.contains("Expect: 100-continue"))
            sock->write("HTTP/1.1 100 Continue\r\n\r\n");
        sock->flush();

        if (m_reply != Reply::Created && m_reply != Reply::ContinueThenClose) {
            // A complete, well-formed, non-2xx response. curl reports CURLE_OK
            // for these responses, so the final HTTP status decides the result --
            // but only if it reaches curl, which is what drainBody() protects.
            m_bodyBytes = drainBody(sock, head, timeoutMs);
            QByteArray status;
            switch (m_reply) {
            case Reply::ServiceUnavailable:
                status = "503 Service Unavailable";
                break;
            case Reply::Unauthorized:
                status = "401 Unauthorized";
                break;
            case Reply::Forbidden:
                status = "403 Forbidden";
                break;
            case Reply::ProxyAuthenticationRequired:
                status = "407 Proxy Authentication Required";
                break;
            case Reply::InsufficientStorage:
                status = "507 Insufficient Storage";
                break;
            default:
                break;
            }
            sock->write("HTTP/1.1 " + status + "\r\nContent-Length: 0\r\n\r\n");
            sock->flush();
            sock->waitForBytesWritten(timeoutMs);
            sock->disconnectFromHost();
            sock->deleteLater();
            return head;
        }

        if (m_reply == Reply::ContinueThenClose) {
            // Swallow the body, then close with no final response -- exactly what
            // the proxy did. Draining first is what makes this the clean shutdown
            // the case is meant to reproduce: closing on unread data would be a
            // reset, i.e. a transport error, which is a different bug entirely.
            m_bodyBytes = drainBody(sock, head, timeoutMs);
            sock->disconnectFromHost();
            sock->deleteLater();
            return head;
        }

        // Success path: consume the declared body, then answer 201.
        m_bodyBytes = drainBody(sock, head, timeoutMs);
        sock->write("HTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n");
        sock->flush();
        sock->waitForBytesWritten(timeoutMs);
        sock->disconnectFromHost();
        sock->deleteLater();
        return head;
    }

    int bodyBytes() const { return m_bodyBytes; }

    // Reads the request body to its end before the caller answers or hangs up.
    //
    // This is not politeness, it is TCP: closing a socket that still holds
    // unread received data sends an RST rather than a FIN, and an RST discards
    // whatever the peer has not yet read -- including the HTTP response written
    // a microsecond earlier. curl then reports a transport error instead of the
    // status, so the provider never gets to map 401/403/507 onto a specific
    // StreamError and answers "Other". Whether curl read the response before the
    // reset arrived decided which sub-case failed, which is why the same binary
    // failed differently on every run.
    //
    // Returns the number of body bytes consumed.
    static int drainBody(QTcpSocket *sock, const QByteArray &head, int timeoutMs) {
        const int alreadyRead = head.size() - (head.indexOf("\r\n\r\n") + 4);
        int have = alreadyRead > 0 ? alreadyRead : 0;
        const int want = contentLength(head);
        if (want >= 0) {
            while (have < want && sock->waitForReadyRead(timeoutMs))
                have += sock->readAll().size();
            return have;
        }
        // Chunked: there is no declared length, so read until the terminating
        // zero-length chunk. The bounded wait keeps a peer that never sends it
        // from hanging the test.
        QByteArray tail = head.mid(head.indexOf("\r\n\r\n") + 4);
        while (!tail.endsWith("0\r\n\r\n") && sock->waitForReadyRead(timeoutMs)) {
            const QByteArray more = sock->readAll();
            have += more.size();
            tail += more;
            if (tail.size() > 4096)
                tail = tail.right(4096); // only the terminator needs to stay visible
        }
        return have;
    }

    static int contentLength(const QByteArray &head) {
        const int i = head.indexOf("Content-Length:");
        if (i < 0)
            return -1;
        return head.mid(i + 15, head.indexOf("\r\n", i) - i - 15).trimmed().toInt();
    }

private:
    QTcpServer m_server;
    Reply m_reply;
    int m_bodyBytes = 0;
};

} // namespace

// The headline data-integrity contract: an upload the server never acknowledged
// must NOT be reported as committed. Before the fix this returned true (curl
// reported CURLE_OK for the 100-then-close) and the app showed the copy as
// finished while the server held nothing.
TEST(WebDavChunkedUploadTest, ContinueThenCloseIsReportedAsFailure) {
    ScriptedHttpServer server(ScriptedHttpServer::Reply::ContinueThenClose);
    ASSERT_GT(server.port(), 0);

    CurlWebDavProvider dav;
    dav.setTimeoutMs(15000);

    const QByteArray payload(64 * 1024, 'x');
    bool committed = true;

    QThread *worker = QThread::create([&]() {
        // openWrite() needs the provider to hold connection settings. Set them
        // via connectToHost against the same scripted port; its PROPFIND is
        // answered by the same script, and its result is not what is under
        // test here -- the PUT is.
        QString err;
        dav.connectToHost("127.0.0.1", server.port(), QStringLiteral("u"),
                          QStringLiteral("p"), false, &err);
    });
    worker->start();
    server.serveHandshake();
    worker->wait(20000);
    delete worker;

    if (!dav.isConnected())
        GTEST_SKIP() << "scripted server did not complete the connect handshake";

    const int curlTransfersBefore = fc::runtimeSnapshot().curlTransfers;
    QThread *put = QThread::create([&]() {
        FileHandle *h = dav.openWrite(QStringLiteral("/big.bin"), true);
        ASSERT_NE(h, nullptr);
        dav.write(h, payload.constData(), payload.size());
        committed = dav.closeHandleStatus(h);
    });
    put->start();
    QElapsedTimer counterWait;
    counterWait.start();
    while (fc::runtimeSnapshot().curlTransfers == curlTransfersBefore &&
           counterWait.elapsed() < 4000) {
        QThread::msleep(1);
    }
    EXPECT_EQ(fc::runtimeSnapshot().curlTransfers, curlTransfersBefore + 1);
    server.serveOnce();
    put->wait(20000);
    delete put;
    EXPECT_EQ(fc::runtimeSnapshot().curlTransfers, curlTransfersBefore);

    // The whole point: no final response means the bytes did not land.
    EXPECT_FALSE(committed) << "upload with no server acknowledgement reported success";
}

// The sharper version of the same contract, and the one that actually pins the
// fix down. A 503 is a *complete* HTTP response, so curl returns CURLE_OK and
// the old code -- which judged success on the CURLcode alone -- reported the
// upload committed. Nothing was stored. Only checking the response code catches
// it, which is exactly what the real proxy's "100 Continue then close" did.
TEST(WebDavChunkedUploadTest, NonSuccessResponseIsReportedAsFailure) {
    ScriptedHttpServer server(ScriptedHttpServer::Reply::ServiceUnavailable);
    ASSERT_GT(server.port(), 0);

    CurlWebDavProvider dav;
    dav.setTimeoutMs(15000);

    QThread *worker = QThread::create([&]() {
        QString err;
        dav.connectToHost("127.0.0.1", server.port(), QStringLiteral("u"),
                          QStringLiteral("p"), false, &err);
    });
    worker->start();
    server.serveHandshake();
    worker->wait(20000);
    delete worker;

    if (!dav.isConnected())
        GTEST_SKIP() << "scripted server did not complete the connect handshake";

    const QByteArray payload(64 * 1024, 'x');
    CloseHandleResult result;
    QThread *put = QThread::create([&]() {
        FileHandle *h = dav.openWrite(QStringLiteral("/rejected.bin"), true);
        ASSERT_NE(h, nullptr);
        dav.setExpectedWriteSize(h, payload.size());
        dav.write(h, payload.constData(), payload.size());
        result = dav.closeHandleResult(h);
    });
    put->start();
    server.serveOnce();
    put->wait(20000);
    delete put;

    EXPECT_FALSE(result.committed) << "a 503 upload was reported as successfully committed";
    EXPECT_EQ(result.error, FileHandle::StreamError::Other);
    EXPECT_EQ(result.detail, QStringLiteral("HTTP 503"));
}

TEST(WebDavChunkedUploadTest, HttpStatusReportsSpecificStreamError) {
    const auto uploadResult = [](ScriptedHttpServer::Reply reply) {
        ScriptedHttpServer server(reply);
        CurlWebDavProvider dav;
        dav.setTimeoutMs(15000);

        QThread *connect = QThread::create([&]() {
            QString error;
            dav.connectToHost("127.0.0.1", server.port(), QStringLiteral("u"),
                              QStringLiteral("p"), false, &error);
        });
        connect->start();
        server.serveHandshake();
        connect->wait(20000);
        delete connect;
        if (!dav.isConnected())
            return CloseHandleResult{false, FileHandle::StreamError::Other,
                                     QStringLiteral("connection failed")};

        CloseHandleResult result;
        QThread *put = QThread::create([&]() {
            const QByteArray payload(64 * 1024, 'x');
            FileHandle *h = dav.openWrite(QStringLiteral("/rejected.bin"), true);
            if (!h) {
                result = {false, FileHandle::StreamError::Other,
                          QStringLiteral("openWrite failed")};
                return;
            }
            dav.setExpectedWriteSize(h, payload.size());
            dav.write(h, payload.constData(), payload.size());
            result = dav.closeHandleResult(h);
        });
        put->start();
        server.serveOnce();
        put->wait(20000);
        delete put;
        return result;
    };

    const auto noSpace = uploadResult(ScriptedHttpServer::Reply::InsufficientStorage);
    EXPECT_FALSE(noSpace.committed);
    EXPECT_EQ(noSpace.error, FileHandle::StreamError::NoSpace);

    const auto unauthorized = uploadResult(ScriptedHttpServer::Reply::Unauthorized);
    EXPECT_FALSE(unauthorized.committed);
    EXPECT_EQ(unauthorized.error, FileHandle::StreamError::PermissionDenied);

    const auto forbidden = uploadResult(ScriptedHttpServer::Reply::Forbidden);
    EXPECT_FALSE(forbidden.committed);
    EXPECT_EQ(forbidden.error, FileHandle::StreamError::PermissionDenied);

    const auto proxyAuth = uploadResult(ScriptedHttpServer::Reply::ProxyAuthenticationRequired);
    EXPECT_FALSE(proxyAuth.committed);
    EXPECT_EQ(proxyAuth.error, FileHandle::StreamError::PermissionDenied);
}

// setExpectedWriteSize() must turn the PUT into a Content-Length request rather
// than a chunked one. This is what makes the upload survive a proxy that
// refuses chunked bodies.
TEST(WebDavChunkedUploadTest, DeclaredSizeSendsContentLengthNotChunked) {
    ScriptedHttpServer server(ScriptedHttpServer::Reply::Created);
    ASSERT_GT(server.port(), 0);

    CurlWebDavProvider dav;
    dav.setTimeoutMs(15000);

    QThread *worker = QThread::create([&]() {
        QString err;
        dav.connectToHost("127.0.0.1", server.port(), QStringLiteral("u"),
                          QStringLiteral("p"), false, &err);
    });
    worker->start();
    server.serveHandshake();
    worker->wait(20000);
    delete worker;

    if (!dav.isConnected())
        GTEST_SKIP() << "scripted server did not complete the connect handshake";

    const QByteArray payload(64 * 1024, 'x');
    QThread *put = QThread::create([&]() {
        FileHandle *h = dav.openWrite(QStringLiteral("/sized.bin"), true);
        ASSERT_NE(h, nullptr);
        dav.setExpectedWriteSize(h, payload.size());
        dav.write(h, payload.constData(), payload.size());
        dav.closeHandleStatus(h);
    });
    put->start();
    const QByteArray head = server.serveOnce();
    put->wait(20000);
    delete put;

    ASSERT_FALSE(head.isEmpty()) << "no request reached the server";
    EXPECT_FALSE(head.contains("Transfer-Encoding: chunked"))
        << "declared size still went out chunked";
    EXPECT_EQ(ScriptedHttpServer::contentLength(head), payload.size());
    EXPECT_EQ(server.bodyBytes(), payload.size());
}
