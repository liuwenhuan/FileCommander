#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>

#include <atomic>
#include <memory>

#include "FileProvider.h"
#include "NetworkSession.h"

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

TEST(NetworkSessionLifecycle, ShutdownAsyncReturnsWhileWorkerStalled) {
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
