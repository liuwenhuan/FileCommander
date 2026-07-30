#include <gtest/gtest.h>

#include <QImage>
#include <QMetaObject>
#include <QPixmap>

#include "ThumbnailCache.h"

namespace {

class ThumbnailMemoryBudget : public ::testing::Test {
protected:
    void TearDown() override {
        ThumbnailCache::instance().setMemoryBudgetKiBForTest(ThumbnailCache::kMemoryBudgetKiB);
    }
};

} // namespace

TEST_F(ThumbnailMemoryBudget, ChargesPixmapBytesUsingItsActualDimensionsAndDepth) {
    QPixmap pixmap(192, 192);
    pixmap.fill(Qt::red);

    EXPECT_GE(ThumbnailCache::pixmapCostKiB(pixmap), 144);
}

TEST_F(ThumbnailMemoryBudget, EvictsPixmapEntriesToStayWithinTheConfiguredByteBudget) {
    ThumbnailCache &cache = ThumbnailCache::instance();
    cache.setMemoryBudgetKiBForTest(1024);
    cache.insertPixmapForTest(QStringLiteral("a"), QPixmap(512, 512));
    cache.insertPixmapForTest(QStringLiteral("b"), QPixmap(512, 512));

    const ThumbnailCache::ThumbnailMemoryStats stats = cache.memoryStatsForTest();
    EXPECT_EQ(stats.entries, 1);
    EXPECT_LE(stats.estimatedBytes, qint64(1024) * 1024);
}

TEST_F(ThumbnailMemoryBudget, RetainsMoreSmallThumbnailsThanLargeThumbnailsAtTheSameBudget) {
    ThumbnailCache &cache = ThumbnailCache::instance();
    cache.setMemoryBudgetKiBForTest(3072);
    for (int i = 0; i < 6; ++i)
        cache.insertPixmapForTest(QStringLiteral("small-%1").arg(i), QPixmap(192, 192));
    const int smallEntries = cache.memoryStatsForTest().entries;

    cache.setMemoryBudgetKiBForTest(3072);
    for (int i = 0; i < 6; ++i)
        cache.insertPixmapForTest(QStringLiteral("large-%1").arg(i), QPixmap(768, 768));
    const int largeEntries = cache.memoryStatsForTest().entries;

    EXPECT_EQ(smallEntries, 6);
    EXPECT_EQ(largeEntries, 1);
}

TEST_F(ThumbnailMemoryBudget, PersistOnlyCompletionDoesNotRetainADecodedPixmap) {
    ThumbnailCache &cache = ThumbnailCache::instance();
    cache.setMemoryBudgetKiBForTest(1024);
    QImage image(192, 192, QImage::Format_RGB32);
    image.fill(Qt::blue);

    ASSERT_TRUE(QMetaObject::invokeMethod(&cache, "storeResult", Qt::DirectConnection,
                                          Q_ARG(QString, QStringLiteral("remote.png")),
                                          Q_ARG(QString, QStringLiteral("pending")),
                                          Q_ARG(QString, QStringLiteral("memory")),
                                          Q_ARG(QImage, image), Q_ARG(bool, false)));
    EXPECT_EQ(cache.memoryStatsForTest().entries, 0);
}
