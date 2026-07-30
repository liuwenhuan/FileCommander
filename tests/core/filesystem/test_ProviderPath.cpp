#include <gtest/gtest.h>

#include <QString>

#include "ProviderPath.h"

namespace {

using fc::ProviderPath::normalizeRooted;
using fc::ProviderPath::parent;
using fc::ProviderPath::sibling;

TEST(ProviderPath, NormalizeEmptyAndRelativePathsAtRoot) {
    EXPECT_EQ(normalizeRooted(QString()), QStringLiteral("/"));
    EXPECT_EQ(normalizeRooted(QStringLiteral("/")), QStringLiteral("/"));
    EXPECT_EQ(normalizeRooted(QStringLiteral("share/file")), QStringLiteral("/share/file"));
}

TEST(ProviderPath, NormalizeCollapsesSlashAndDotComponents) {
    EXPECT_EQ(normalizeRooted(QStringLiteral("//share///dir/./file/")),
              QStringLiteral("/share/dir/file"));
}

TEST(ProviderPath, NormalizeResolvesDotDotAndClampsAtRoot) {
    EXPECT_EQ(normalizeRooted(QStringLiteral("/share/dir/../file")),
              QStringLiteral("/share/file"));
    EXPECT_EQ(normalizeRooted(QStringLiteral("/../../share")), QStringLiteral("/share"));
    EXPECT_EQ(normalizeRooted(QStringLiteral("/share/../../../")), QStringLiteral("/"));
}

TEST(ProviderPath, NormalizePreservesUnicodeAndNonSlashCharacters) {
    EXPECT_EQ(normalizeRooted(QStringLiteral("//share///目录/./文件.txt")),
              QStringLiteral("/share/目录/文件.txt"));
    EXPECT_EQ(normalizeRooted(QStringLiteral("/a\\b/%E7%9B%AE%E5%BD%95")),
              QStringLiteral("/a\\b/%E7%9B%AE%E5%BD%95"));
}

TEST(ProviderPath, ParentOfRootOrEmptyIsEmpty) {
    EXPECT_TRUE(parent(QStringLiteral("/")).isEmpty());
    EXPECT_TRUE(parent(QString()).isEmpty());
}

TEST(ProviderPath, ParentReturnsRootedNormalizedParent) {
    EXPECT_EQ(parent(QStringLiteral("/share")), QStringLiteral("/"));
    EXPECT_EQ(parent(QStringLiteral("/share/dir/file")), QStringLiteral("/share/dir"));
    EXPECT_EQ(parent(QStringLiteral("//share/目录/./file/../child/")),
              QStringLiteral("/share/目录"));
}

TEST(ProviderPath, SiblingReplacesFinalComponent) {
    EXPECT_EQ(sibling(QStringLiteral("/share/old"), QStringLiteral("new")),
              QStringLiteral("/share/new"));
    EXPECT_EQ(sibling(QStringLiteral("/old"), QStringLiteral("新文件.txt")),
              QStringLiteral("/新文件.txt"));
    EXPECT_EQ(sibling(QStringLiteral("//share/dir/../old"), QStringLiteral("new")),
              QStringLiteral("/share/new"));
}

TEST(ProviderPath, SiblingRejectsPathsWithoutFinalComponent) {
    EXPECT_TRUE(sibling(QStringLiteral("/"), QStringLiteral("new")).isEmpty());
    EXPECT_TRUE(sibling(QString(), QStringLiteral("new")).isEmpty());
}

TEST(ProviderPath, SiblingRejectsInvalidNames) {
    const QString path = QStringLiteral("/share/old");
    EXPECT_TRUE(sibling(path, QString()).isEmpty());
    EXPECT_TRUE(sibling(path, QStringLiteral(".")).isEmpty());
    EXPECT_TRUE(sibling(path, QStringLiteral("..")).isEmpty());
    EXPECT_TRUE(sibling(path, QStringLiteral("dir/new")).isEmpty());
    EXPECT_TRUE(sibling(path, QStringLiteral("dir\\new")).isEmpty());
}

} // namespace
