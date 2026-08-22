#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>

#include "filesystem/FileProvider.h"
#include "text/TextEncodingIdentity.h"

TEST(TextEncodingIdentityTest, LocalIdentityNormalizesEquivalentPaths) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("note.txt"));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.close();

    EXPECT_EQ(fc::TextEncodingIdentity::localPath(path),
              fc::TextEncodingIdentity::localPath(
                  dir.filePath(QStringLiteral("sub/../note.txt"))));
}

TEST(TextEncodingIdentityTest, RemoteIdentityIncludesConnectionButNeverPassword) {
    RemoteLocation first;
    first.scheme = QStringLiteral("sftp");
    first.host = QStringLiteral("Example.COM");
    first.port = 2222;
    first.user = QStringLiteral("alice");
    first.password = QStringLiteral("secret-one");

    RemoteLocation same = first;
    same.password = QStringLiteral("secret-two");
    EXPECT_EQ(fc::TextEncodingIdentity::remotePath(first, QStringLiteral("/docs/./note.txt")),
              fc::TextEncodingIdentity::remotePath(same, QStringLiteral("/docs/note.txt")));

    RemoteLocation other = first;
    other.port = 22;
    EXPECT_NE(fc::TextEncodingIdentity::remotePath(first, QStringLiteral("/docs/note.txt")),
              fc::TextEncodingIdentity::remotePath(other, QStringLiteral("/docs/note.txt")));
    other = first;
    other.user = QStringLiteral("bob");
    EXPECT_NE(fc::TextEncodingIdentity::remotePath(first, QStringLiteral("/docs/note.txt")),
              fc::TextEncodingIdentity::remotePath(other, QStringLiteral("/docs/note.txt")));
}

TEST(TextEncodingIdentityTest, ArchiveEntryIncludesItsContainer) {
    const QString entry = QStringLiteral("/inside/readme.txt");
    const QString first = fc::TextEncodingIdentity::archiveEntry(
        QStringLiteral("container-one"), entry);
    EXPECT_EQ(first, fc::TextEncodingIdentity::archiveEntry(
                         QStringLiteral("container-one"), QStringLiteral("/inside/./readme.txt")));
    EXPECT_NE(first, fc::TextEncodingIdentity::archiveEntry(
                         QStringLiteral("container-two"), entry));
    EXPECT_NE(first, fc::TextEncodingIdentity::archiveEntry(
                         QStringLiteral("container-one"), QStringLiteral("/other/readme.txt")));
}
