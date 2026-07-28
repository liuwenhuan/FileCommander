#include <gtest/gtest.h>

#include "network/WindowsSmbProvider.h"

TEST(WindowsSmbProvider, ConvertsProviderPathsToCanonicalUnc) {
    QString error;
    EXPECT_EQ(WindowsSmbProvider::providerPathToUnc(
                  QStringLiteral("nas"), QStringLiteral("/"), &error),
              QStringLiteral("\\\\nas"));
    EXPECT_EQ(WindowsSmbProvider::providerPathToUnc(
                  QStringLiteral("nas"), QStringLiteral("/共享/目录/文件.txt"), &error),
              QStringLiteral("\\\\nas\\共享\\目录\\文件.txt"));
    EXPECT_TRUE(error.isEmpty());
}

TEST(WindowsSmbProvider, RejectsTraversalAndInvalidHosts) {
    QString error;
    EXPECT_TRUE(WindowsSmbProvider::providerPathToUnc(
                    QStringLiteral("nas"), QStringLiteral("/share/../secret"), &error)
                    .isEmpty());
    EXPECT_FALSE(error.isEmpty());

    error.clear();
    EXPECT_TRUE(WindowsSmbProvider::providerPathToUnc(
                    QStringLiteral("nas\\other"), QStringLiteral("/share"), &error)
                    .isEmpty());
    EXPECT_FALSE(error.isEmpty());
}

TEST(WindowsSmbProvider, NormalizesProviderPathWithoutEscapingRoot) {
    EXPECT_EQ(WindowsSmbProvider::normalizeProviderPath(QStringLiteral("//share///dir/")),
              QStringLiteral("/share/dir"));
    EXPECT_EQ(WindowsSmbProvider::parentProviderPath(QStringLiteral("/share/dir")),
              QStringLiteral("/share"));
    EXPECT_TRUE(WindowsSmbProvider::parentProviderPath(QStringLiteral("/")).isEmpty());
}
