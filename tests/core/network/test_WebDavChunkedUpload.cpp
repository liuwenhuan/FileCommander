#include <gtest/gtest.h>

#include <QByteArray>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>

#include "CurlWebDavProvider.h"

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
        ServiceUnavailable // a complete, well-formed, non-2xx response
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

        if (m_reply == Reply::ServiceUnavailable) {
            // A complete, well-formed response that simply isn't a success.
            // curl reports CURLE_OK for this -- there is no transport error --
            // so only the response code distinguishes it from a stored file.
            sock->waitForReadyRead(500);
            sock->readAll();
            sock->write("HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n");
            sock->flush();
            sock->waitForBytesWritten(timeoutMs);
            sock->disconnectFromHost();
            sock->deleteLater();
            return head;
        }

        if (m_reply == Reply::ContinueThenClose) {
            // Drain briefly, then close with no final response -- exactly what
            // the proxy did. curl sees a clean shutdown, not an error.
            sock->waitForReadyRead(500);
            sock->readAll();
            sock->disconnectFromHost();
            sock->deleteLater();
            return head;
        }

        // Success path: consume the declared body, then answer 201.
        const int want = contentLength(head);
        QByteArray body = head.mid(head.indexOf("\r\n\r\n") + 4);
        while (want > 0 && body.size() < want && sock->waitForReadyRead(timeoutMs))
            body += sock->readAll();
        m_bodyBytes = body.size();
        sock->write("HTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n");
        sock->flush();
        sock->waitForBytesWritten(timeoutMs);
        sock->disconnectFromHost();
        sock->deleteLater();
        return head;
    }

    int bodyBytes() const { return m_bodyBytes; }

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

    QThread *put = QThread::create([&]() {
        FileHandle *h = dav.openWrite(QStringLiteral("/big.bin"), true);
        ASSERT_NE(h, nullptr);
        dav.write(h, payload.constData(), payload.size());
        committed = dav.closeHandleStatus(h);
    });
    put->start();
    server.serveOnce();
    put->wait(20000);
    delete put;

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
    bool committed = true;
    QThread *put = QThread::create([&]() {
        FileHandle *h = dav.openWrite(QStringLiteral("/rejected.bin"), true);
        ASSERT_NE(h, nullptr);
        dav.setExpectedWriteSize(h, payload.size());
        dav.write(h, payload.constData(), payload.size());
        committed = dav.closeHandleStatus(h);
    });
    put->start();
    server.serveOnce();
    put->wait(20000);
    delete put;

    EXPECT_FALSE(committed) << "a 503 upload was reported as successfully committed";
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
