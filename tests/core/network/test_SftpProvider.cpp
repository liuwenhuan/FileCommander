#include <gtest/gtest.h>

#include <QDir>

#include <QString>

#include "SftpProvider.h"

// These tests exercise only the POSIX path logic (cleanPath / parentPath),
// which is pure and needs no live connection. Connect/list/stat/rename require
// a real SFTP server and are not covered here.

// --- cleanPath ----------------------------------------------------------

TEST(SftpProviderTest, Path_CleanCollapsesRedundantSlashes) {
    SftpProvider p;
    EXPECT_EQ(p.cleanPath("/srv//data///x"), QString("/srv/data/x"));
}

TEST(SftpProviderTest, Path_CleanResolvesDotAndDotDot) {
    SftpProvider p;
    EXPECT_EQ(p.cleanPath("/srv/./data/../logs"), QString("/srv/logs"));
    EXPECT_EQ(p.cleanPath("/a/b/c/../../d"), QString("/a/d"));
}

TEST(SftpProviderTest, Path_CleanDotDotAtRootStaysAtRoot) {
    SftpProvider p;
    EXPECT_EQ(p.cleanPath("/.."), QString("/"));
    EXPECT_EQ(p.cleanPath("/../../x"), QString("/x"));
}

TEST(SftpProviderTest, Path_CleanEmptyOrRelativeFallsBackToRoot) {
    SftpProvider p;
    EXPECT_EQ(p.cleanPath(""), QString("/"));
    EXPECT_EQ(p.cleanPath("."), QString("/"));
    // Relative input is rooted (POSIX absolutisation for a remote browse root).
    EXPECT_EQ(p.cleanPath("srv/data"), QString("/srv/data"));
}

TEST(SftpProviderTest, Path_CleanTrailingSlashRemoved) {
    SftpProvider p;
    EXPECT_EQ(p.cleanPath("/srv/data/"), QString("/srv/data"));
    EXPECT_EQ(p.cleanPath("/"), QString("/"));
}

TEST(SftpProviderTest, Path_CleanIsAlwaysPosixRegardlessOfBackslashes) {
    SftpProvider p;
    // Backslashes are ordinary filename characters on POSIX, not separators.
    EXPECT_EQ(p.cleanPath("/srv/a\\b"), QString("/srv/a\\b"));
}

// --- parentPath ---------------------------------------------------------

TEST(SftpProviderTest, Path_ParentOfNestedPath) {
    SftpProvider p;
    EXPECT_EQ(p.parentPath("/srv/data/x"), QString("/srv/data"));
}

TEST(SftpProviderTest, Path_ParentOfTopLevelIsRoot) {
    SftpProvider p;
    EXPECT_EQ(p.parentPath("/srv"), QString("/"));
}

TEST(SftpProviderTest, Path_ParentOfRootIsEmpty) {
    SftpProvider p;
    EXPECT_EQ(p.parentPath("/"), QString());
}

TEST(SftpProviderTest, Path_ParentNormalisesBeforeSplitting) {
    SftpProvider p;
    EXPECT_EQ(p.parentPath("/srv/data/"), QString("/srv"));
    EXPECT_EQ(p.parentPath("/srv/./data/../x"), QString("/srv"));
}

// --- disconnected state -------------------------------------------------

TEST(SftpProviderTest, State_FreshProviderIsNotConnected) {
    SftpProvider p;
    EXPECT_FALSE(p.isConnected());
    EXPECT_TRUE(p.host().isEmpty());
    // Off-line queries degrade gracefully rather than crashing.
    EXPECT_FALSE(p.exists("/anything"));
    EXPECT_FALSE(p.isDir("/anything"));
    EXPECT_TRUE(p.list("/anything", true).isEmpty());
}

// --- server-side move ---------------------------------------------------

TEST(SftpProviderTest, Move_DisconnectedReportsUnsupportedNotFailure) {
    SftpProvider p;
    // Unsupported means "ask someone else"; Failed would make the transfer
    // engine treat a missing connection as a real error instead of falling
    // back to streaming.
    EXPECT_EQ(p.moveTo("/a/x.txt", "/b/x.txt"), FileProvider::RenameResult::Unsupported);
}

TEST(SftpProviderTest, Move_RefusesMoveOntoItself) {
    SftpProvider p;
    // Same source and destination would otherwise risk a backend deleting the
    // only copy; rejected before any connection check.
    EXPECT_EQ(p.moveTo("/a/x.txt", "/a/./x.txt"), FileProvider::RenameResult::Failed);
}

// Public-key authentication looked for keys under $HOME. Windows does not set
// HOME, so every candidate came out as "/.ssh/id_rsa" -- a path that exists
// nowhere -- and key auth could not succeed there at all.
TEST(SftpProviderTest, SftpKeyPathsTest_KeysAreLookedForUnderTheRealHomeDirectory) {
    const QStringList paths = SftpProvider::defaultPrivateKeyPaths();
    ASSERT_FALSE(paths.isEmpty());

    const QString home = QDir::homePath();
    ASSERT_FALSE(home.isEmpty());
    for (const QString &path : paths) {
        SCOPED_TRACE(path.toStdString());
        EXPECT_TRUE(path.startsWith(home))
            << "not under the home directory, so nothing will ever be found there";
        EXPECT_TRUE(path.contains(QStringLiteral(".ssh")));
        // The shape the bug produced: an empty home leaves the path rooted at
        // the filesystem root.
        EXPECT_FALSE(path.startsWith(QStringLiteral("/.ssh")));
    }
    EXPECT_TRUE(paths.first().endsWith(QStringLiteral("id_ed25519")))
        << "the strongest key type should be tried first";
}
