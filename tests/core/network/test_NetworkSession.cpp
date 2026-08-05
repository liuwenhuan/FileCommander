#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>

#include <atomic>
#include <memory>

#include "FileProvider.h"
#include "NetworkSession.h"
#include "diagnostics/RuntimeCounters.h"

// These tests pin the P1.5 contract: tearing a NetworkSession down must never
// block the calling (GUI) thread, even when its worker is stuck in a slow dial.
// They use the QApplication event loop set up in test_main.cpp.

namespace {

// A do-nothing provider: NetworkSession's connect path runs the supplied
// ConnectFn, not the provider, so these trivial overrides are enough.
class StubProvider : public FileProvider {
public:
    QVector<FileInfo> list(const QString &, bool) const override { return {}; }
    bool isDir(const QString &) const override { return true; }
    QString cleanPath(const QString &p) const override { return p; }
    QString parentPath(const QString &) const override { return {}; }
    bool exists(const QString &) const override { return false; }
    RenameResult rename(const QString &, const QString &, QString *) override {
        return RenameResult::Failed;
    }
    // Lets a test stand in for a backend whose connect was rejected by the server
    // (SMB's EACCES, SFTP's userauth failure, ...).
    void setAuthFailed(bool failed) { m_lastConnectAuthFailed = failed; }
};

// Drives the calling thread's event loop until `pred` holds or `budgetMs`
// elapses. Returns whether the predicate became true.
template <typename Pred>
bool spinUntil(Pred pred, int budgetMs) {
    QElapsedTimer t;
    t.start();
    while (!pred() && t.elapsed() < budgetMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return pred();
}

} // namespace

TEST(NetworkSessionTest, Lifecycle_ShutdownAsyncReturnsWhileWorkerStalled) {
    std::atomic<bool> connectEntered{false};
    std::atomic<bool> destroyed{false};

    auto provider = std::make_shared<StubProvider>();
    // The exact shared_ptr shape connectNetwork installs: the last owner drop runs
    // shutdownAsync instead of a blocking delete.
    std::shared_ptr<NetworkSession> session(new NetworkSession(provider),
                                            [](NetworkSession *s) { s->shutdownAsync(); });

    // Fires on the worker thread when the object is finally torn down.
    QObject::connect(session.get(), &QObject::destroyed,
                     [&destroyed] { destroyed = true; });

    // Connect closure blocks the worker for ~1.5s, standing in for a stalled dial.
    session->start(
        [&connectEntered](QString *) -> bool {
            connectEntered = true;
            QThread::msleep(1500);
            return false;
        },
        QStringLiteral("/"));

    ASSERT_TRUE(spinUntil([&] { return connectEntered.load(); }, 3000))
        << "worker never entered the blocking connect";

    // Drop the last owner mid-stall: shutdownAsync must return effectively
    // immediately, NOT wait out the 1.5s blocking connect.
    QElapsedTimer teardown;
    teardown.start();
    session.reset();
    EXPECT_LT(teardown.elapsed(), 300)
        << "teardown blocked the caller for " << teardown.elapsed() << "ms";

    // And the session is eventually reaped once the stalled connect unwinds -- no
    // leak, no hang.
    EXPECT_TRUE(spinUntil([&] { return destroyed.load(); }, 6000))
        << "session was never destroyed after the worker unwound";
}

TEST(NetworkSessionTest, Lifecycle_CountersCoverConnectedSessionAndReturnToBaseline) {
    const fc::RuntimeSnapshot before = fc::runtimeSnapshot();
    std::atomic<bool> connected{false};
    std::atomic<bool> destroyed{false};

    auto provider = std::make_shared<StubProvider>();
    std::shared_ptr<NetworkSession> session(new NetworkSession(provider),
                                            [](NetworkSession *s) { s->shutdownAsync(); });
    QObject::connect(session.get(), &QObject::destroyed, [&destroyed] { destroyed = true; });
    QObject::connect(session.get(), &NetworkSession::stateChanged, [&connected](int state, int) {
        if (state == NetworkSession::Connected)
            connected = true;
    });

    session->start([](QString *) { return true; }, QStringLiteral("/"));
    ASSERT_TRUE(spinUntil([&] { return connected.load(); }, 3000));

    const fc::RuntimeSnapshot active = fc::runtimeSnapshot();
    EXPECT_EQ(active.networkSessions, before.networkSessions + 1);
    EXPECT_EQ(active.networkThreads, before.networkThreads + 1);
    ASSERT_TRUE(spinUntil(
        [&] {
            return fc::runtimeSnapshot().activeHeartbeats == before.activeHeartbeats + 1;
        },
        3000));

    session.reset();
    ASSERT_TRUE(spinUntil([&] { return destroyed.load(); }, 3000));
    const fc::RuntimeSnapshot after = fc::runtimeSnapshot();
    EXPECT_EQ(after.networkSessions, before.networkSessions);
    EXPECT_EQ(after.networkThreads, before.networkThreads);
    EXPECT_EQ(after.activeHeartbeats, before.activeHeartbeats);
}

// One server rejection must raise exactly ONE credential prompt. The tab issues
// a list request immediately after connectNetwork() (FilePanel::navigateTo), and
// that request lands on the worker right after the connect was rejected -- it
// must not re-dial and emit a second authRequired, which showed the user two
// password dialogs for a single login.
TEST(NetworkSessionTest, Auth_RejectedConnectPromptsOnceDespiteQueuedListRequest) {
    auto provider = std::make_shared<StubProvider>();
    std::shared_ptr<NetworkSession> session(new NetworkSession(provider),
                                            [](NetworkSession *s) { s->shutdownAsync(); });

    std::atomic<int> authPrompts{0};
    std::atomic<int> connectCalls{0};
    QObject::connect(session.get(), &NetworkSession::authRequired,
                     [&authPrompts](const QString &) { ++authPrompts; });

    session->start(
        [provider, &connectCalls](QString *error) {
            ++connectCalls;
            provider->setAuthFailed(true); // the server wants credentials
            *error = QStringLiteral("Authentication required");
            return false;
        },
        QStringLiteral("/"));
    // Exactly what the panel does right after connectNetwork().
    session->requestList(1, QStringLiteral("/"), false);

    ASSERT_TRUE(spinUntil([&] { return authPrompts.load() >= 1; }, 5000))
        << "the rejected connect never asked for credentials";
    // Give the queued list request room to (wrongly) trigger a second dial.
    spinUntil([] { return false; }, 1000);

    EXPECT_EQ(authPrompts.load(), 1) << "the user was shown more than one password dialog";
    EXPECT_EQ(connectCalls.load(), 1) << "the queued list request re-dialled while awaiting a login";
}

// The guard above must not swallow a genuine second rejection: a wrong password
// has to prompt again, and the right one then connects.
TEST(NetworkSessionTest, Auth_WrongCredentialsPromptAgainAndCorrectOnesConnect) {
    auto provider = std::make_shared<StubProvider>();
    std::shared_ptr<NetworkSession> session(new NetworkSession(provider),
                                            [](NetworkSession *s) { s->shutdownAsync(); });

    std::atomic<int> authPrompts{0};
    std::atomic<bool> connected{false};
    QObject::connect(session.get(), &NetworkSession::stateChanged, [&connected](int state, int) {
        if (state == NetworkSession::Connected)
            connected = true;
    });
    // Mirrors MainWindow: every prompt feeds credentials back via retryWith().
    // The first answer is wrong, the second is accepted.
    QObject::connect(session.get(), &NetworkSession::authRequired, [&](const QString &) {
        const bool correct = ++authPrompts >= 2;
        session->retryWith([provider, correct](QString *error) {
            if (correct)
                return true;
            provider->setAuthFailed(true);
            *error = QStringLiteral("Authentication required");
            return false;
        });
    });

    session->start(
        [provider](QString *error) {
            provider->setAuthFailed(true);
            *error = QStringLiteral("Authentication required");
            return false;
        },
        QStringLiteral("/"));
    session->requestList(1, QStringLiteral("/"), false);

    EXPECT_TRUE(spinUntil([&] { return connected.load(); }, 8000))
        << "the correct password never established the connection";
    EXPECT_EQ(authPrompts.load(), 2) << "a rejected password must prompt again";
}
