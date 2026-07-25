#include <gtest/gtest.h>

#include <QVector>

#include "SmbHostBrowser.h"

namespace {

SmbHost host(const QString &name, const QString &address) { return SmbHost{name, address}; }

const SmbHost *find(const QVector<SmbHost> &hosts, const QString &name) {
    for (const SmbHost &h : hosts) {
        if (h.name.compare(name, Qt::CaseInsensitive) == 0)
            return &h;
    }
    return nullptr;
}

} // namespace

// The reported bug: a NAS advertising over mDNS arrives as a bare name, and the
// TCP-445 sweep later reports the same machine as a bare address. Keeping them
// apart (or dropping the second) left the row reading "ds224plus" with no IP
// for the rest of the session.
TEST(SmbHostMerge, NameThenAddressBecomeOneHostWithBoth) {
    QVector<SmbHost> known;
    EXPECT_TRUE(mergeDiscoveredHosts(known, {host("ds224plus", QString())}));
    ASSERT_EQ(known.size(), 1);
    EXPECT_TRUE(known.first().address.isEmpty());

    EXPECT_TRUE(mergeDiscoveredHosts(known, {host("ds224plus", QStringLiteral("192.168.31.42"))}));
    ASSERT_EQ(known.size(), 1) << "the second sighting was added as a separate row";
    EXPECT_EQ(known.first().name, QStringLiteral("ds224plus"));
    EXPECT_EQ(known.first().address, QStringLiteral("192.168.31.42"));
}

// Same merge, opposite arrival order -- sources finish in whatever order the
// network allows, so neither may assume it went first.
TEST(SmbHostMerge, AddressThenNameBecomeOneHostWithBoth) {
    QVector<SmbHost> known;
    mergeDiscoveredHosts(known, {host(QString(), QStringLiteral("192.168.31.42"))});
    EXPECT_TRUE(mergeDiscoveredHosts(known, {host("ds224plus", QStringLiteral("192.168.31.42"))}));

    ASSERT_EQ(known.size(), 1);
    EXPECT_EQ(known.first().name, QStringLiteral("ds224plus"));
    EXPECT_EQ(known.first().address, QStringLiteral("192.168.31.42"));
}

// A bare name cannot be attached to an address-only entry on sight -- nothing
// yet says they are the same machine, and guessing would mislabel a row. They
// stay separate until a sighting carrying both halves ties them together, at
// which point the redundant address-only row disappears.
TEST(SmbHostMerge, BareNameAndBareAddressOnlyJoinOnceLinked) {
    QVector<SmbHost> known;
    mergeDiscoveredHosts(known, {host(QString(), QStringLiteral("192.168.31.42"))});
    mergeDiscoveredHosts(known, {host("ds224plus", QString())});
    EXPECT_EQ(known.size(), 2) << "unrelated halves were fused on a guess";

    // The sweep now resolves the name for that address: one machine, one row.
    EXPECT_TRUE(mergeDiscoveredHosts(known, {host("ds224plus", QStringLiteral("192.168.31.42"))}));
    ASSERT_EQ(known.size(), 1);
    EXPECT_EQ(known.first().name, QStringLiteral("ds224plus"));
    EXPECT_EQ(known.first().address, QStringLiteral("192.168.31.42"));
}

// Merging must not go so far as to fuse genuinely different machines: two
// cloned installs really do share a hostname, and their addresses say so.
TEST(SmbHostMerge, SameNameDifferentAddressesStaySeparate) {
    QVector<SmbHost> known;
    mergeDiscoveredHosts(known, {host("deepin-PC", QStringLiteral("192.168.31.10"))});
    mergeDiscoveredHosts(known, {host("deepin-PC", QStringLiteral("192.168.31.11"))});

    EXPECT_EQ(known.size(), 2) << "two distinct machines collapsed into one row";
}

// mDNS resolves the same name on IPv4, IPv6 and once per interface; the list
// must show the host once, and repeats must not report a change (which would
// rebuild the view for nothing).
TEST(SmbHostMerge, RepeatedIdenticalSightingsChangeNothing) {
    QVector<SmbHost> known;
    const SmbHost nas = host("ds224plus", QStringLiteral("192.168.31.42"));
    EXPECT_TRUE(mergeDiscoveredHosts(known, {nas}));
    EXPECT_FALSE(mergeDiscoveredHosts(known, {nas, nas}));
    EXPECT_EQ(known.size(), 1);
}

// Hostnames are case-insensitive, and sources disagree about case (NetBIOS
// shouts, mDNS usually doesn't).
TEST(SmbHostMerge, NameMatchIgnoresCase) {
    QVector<SmbHost> known;
    mergeDiscoveredHosts(known, {host("DS224PLUS", QString())});
    mergeDiscoveredHosts(known, {host("ds224plus", QStringLiteral("192.168.31.42"))});

    ASSERT_EQ(known.size(), 1);
    EXPECT_EQ(known.first().address, QStringLiteral("192.168.31.42"));
}

// A batch carrying neither half is not a host and must be ignored rather than
// becoming an unclickable blank row.
TEST(SmbHostMerge, EntriesWithNeitherNameNorAddressAreDropped) {
    QVector<SmbHost> known;
    EXPECT_FALSE(mergeDiscoveredHosts(known, {host(QString(), QString())}));
    EXPECT_TRUE(known.isEmpty());
}

// Several hosts in one batch, mixed shapes -- the realistic sweep result.
TEST(SmbHostMerge, MixedBatchMergesPerHost) {
    QVector<SmbHost> known;
    mergeDiscoveredHosts(known, {host("ds224plus", QString()), host("deepin-PC", QString())});
    mergeDiscoveredHosts(known, {host(QString(), QStringLiteral("192.168.31.42")),
                                 host("ds224plus", QStringLiteral("192.168.31.42")),
                                 host("deepin-PC", QStringLiteral("192.168.31.210"))});

    ASSERT_EQ(known.size(), 2);
    const SmbHost *nas = find(known, QStringLiteral("ds224plus"));
    const SmbHost *pc = find(known, QStringLiteral("deepin-PC"));
    ASSERT_NE(nas, nullptr);
    ASSERT_NE(pc, nullptr);
    EXPECT_EQ(nas->address, QStringLiteral("192.168.31.42"));
    EXPECT_EQ(pc->address, QStringLiteral("192.168.31.210"));
}
