#include <gtest/gtest.h>

#include <QSignalSpy>

#include "PendingSendQueue.h"

// The queue behind "send to a device that is offline": the send is held and
// fired the moment that device next shows as online. Pure QObject, no widgets,
// so the assertions are about the drain logic rather than any network.
namespace {

} // namespace

TEST(PendingSendQueue, ASendForAnOfflineDeviceWaitsUntilItIsOnline) {
    PendingSendQueue queue;

    QSignalSpy queued(&queue, &PendingSendQueue::queued);
    QSignalSpy sendReady(&queue, &PendingSendQueue::sendReady);

    queue.enqueue(QStringLiteral("dev-1"), QStringLiteral("laptop"),
                  {QStringLiteral("/tmp/a.txt")});
    ASSERT_EQ(queued.count(), 1);

    // Still offline: nothing is released.
    queue.devicesChanged({QStringLiteral("dev-2")});
    EXPECT_EQ(sendReady.count(), 0);
    EXPECT_EQ(queue.pendingCount(), 1);

    // Online now: the queued send fires with the values it was enqueued with.
    queue.devicesChanged({QStringLiteral("dev-1")});
    ASSERT_EQ(sendReady.count(), 1);
    EXPECT_EQ(sendReady.first().at(0).toString(), QStringLiteral("dev-1"));
    EXPECT_EQ(sendReady.first().at(1).toString(), QStringLiteral("laptop"));
    EXPECT_EQ(sendReady.first().at(2).toStringList(), QStringList({QStringLiteral("/tmp/a.txt")}));
    EXPECT_EQ(queue.pendingCount(), 0);
}

TEST(PendingSendQueue, EntriesForDevicesThatStayOfflineRemainQueued) {
    PendingSendQueue queue;
    QSignalSpy sendReady(&queue, &PendingSendQueue::sendReady);

    queue.enqueue(QStringLiteral("dev-1"), QStringLiteral("laptop"), {QStringLiteral("/a")});
    queue.enqueue(QStringLiteral("dev-2"), QStringLiteral("desktop"), {QStringLiteral("/b")});

    // Only dev-2 comes online: dev-1's send stays queued, dev-2's fires.
    queue.devicesChanged({QStringLiteral("dev-2")});
    ASSERT_EQ(sendReady.count(), 1);
    EXPECT_EQ(sendReady.first().at(0).toString(), QStringLiteral("dev-2"));
    EXPECT_EQ(queue.pendingCount(), 1);
}
