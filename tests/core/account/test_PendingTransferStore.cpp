#include <gtest/gtest.h>

#include <QTemporaryDir>

#include "account/PendingTransferStore.h"

// The store that remembers device sends across launches. The contract worth
// pinning: an added entry survives a fresh store instance (so a crash still
// offers resume next launch), a duplicate add does not double it, and a remove
// of the exact device+sources pair leaves the rest alone.
namespace {

// Points the config dir at a directory of this test's own, so nothing here can
// read or scribble on the real pending-transfers.json.
class IsolatedConfig {
public:
    IsolatedConfig() { qputenv("FILECOMMANDER_CONFIG_HOME", directory.path().toUtf8()); }
    ~IsolatedConfig() { qunsetenv("FILECOMMANDER_CONFIG_HOME"); }
    QTemporaryDir directory;
};

} // namespace

TEST(PendingTransferStore, AnEntrySurvivesANewInstance) {
    IsolatedConfig config;
    const QStringList sources{QStringLiteral("/home/u/a.bin"), QStringLiteral("/home/u/b.bin")};

    {
        PendingTransferStore store;
        store.add(QStringLiteral("dev-1"), QStringLiteral("laptop"), sources);
    }

    PendingTransferStore reopened;
    const QVector<PendingSend> entries = reopened.entries();
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries.at(0).deviceId, QStringLiteral("dev-1"));
    EXPECT_EQ(entries.at(0).deviceName, QStringLiteral("laptop"));
    EXPECT_EQ(entries.at(0).sources, sources);
}

TEST(PendingTransferStore, ADuplicateAddDoesNotDoubleIt) {
    IsolatedConfig config;
    PendingTransferStore store;
    const QStringList sources{QStringLiteral("/a.bin")};
    store.add(QStringLiteral("dev-1"), QStringLiteral("laptop"), sources);
    store.add(QStringLiteral("dev-1"), QStringLiteral("laptop"), sources);
    EXPECT_EQ(store.entries().size(), 1);
}

TEST(PendingTransferStore, RemovingOnePairLeavesTheRest) {
    IsolatedConfig config;
    PendingTransferStore store;
    store.add(QStringLiteral("dev-1"), QStringLiteral("laptop"), {QStringLiteral("/a.bin")});
    store.add(QStringLiteral("dev-2"), QStringLiteral("desktop"), {QStringLiteral("/b.bin")});

    store.remove(QStringLiteral("dev-1"), {QStringLiteral("/a.bin")});
    const QVector<PendingSend> entries = store.entries();
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries.at(0).deviceId, QStringLiteral("dev-2"));
}
