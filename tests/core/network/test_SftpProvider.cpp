#include <gtest/gtest.h>

#include <QString>

#include "SftpProvider.h"

// These tests exercise only the POSIX path logic (cleanPath / parentPath),
// which is pure and needs no live connection. Connect/list/stat/rename require
// a real SFTP server and are not covered here.

// --- cleanPath ----------------------------------------------------------

TEST(SftpProviderPath, CleanCollapsesRedundantSlashes) {
    SftpProvider p;
    EXPECT_EQ(p.cleanPath("/srv//data///x"), QString("/srv/data/x"));
}

TEST(SftpProviderPath, CleanResolvesDotAndDotDot) {
    SftpProvider p;
    EXPECT_EQ(p.cleanPath("/srv/./data/../logs"), QString("/srv/logs"));
    EXPECT_EQ(p.cleanPath("/a/b/c/../../d"), QString("/a/d"));
}

TEST(SftpProviderPath, CleanDotDotAtRootStaysAtRoot) {
    SftpProvider p;
    EXPECT_EQ(p.cleanPath("/.."), QString("/"));
    EXPECT_EQ(p.cleanPath("/../../x"), QString("/x"));
}

TEST(SftpProviderPath, CleanEmptyOrRelativeFallsBackToRoot) {
    SftpProvider p;
    EXPECT_EQ(p.cleanPath(""), QString("/"));
    EXPECT_EQ(p.cleanPath("."), QString("/"));
    // Relative input is rooted (POSIX absolutisation for a remote browse root).
    EXPECT_EQ(p.cleanPath("srv/data"), QString("/srv/data"));
}

TEST(SftpProviderPath, CleanTrailingSlashRemoved) {
    SftpProvider p;
    EXPECT_EQ(p.cleanPath("/srv/data/"), QString("/srv/data"));
    EXPECT_EQ(p.cleanPath("/"), QString("/"));
}

TEST(SftpProviderPath, CleanIsAlwaysPosixRegardlessOfBackslashes) {
    SftpProvider p;
    // Backslashes are ordinary filename characters on POSIX, not separators.
    EXPECT_EQ(p.cleanPath("/srv/a\\b"), QString("/srv/a\\b"));
}

// --- parentPath ---------------------------------------------------------

TEST(SftpProviderPath, ParentOfNestedPath) {
    SftpProvider p;
    EXPECT_EQ(p.parentPath("/srv/data/x"), QString("/srv/data"));
}

TEST(SftpProviderPath, ParentOfTopLevelIsRoot) {
    SftpProvider p;
    EXPECT_EQ(p.parentPath("/srv"), QString("/"));
}

TEST(SftpProviderPath, ParentOfRootIsEmpty) {
    SftpProvider p;
    EXPECT_EQ(p.parentPath("/"), QString());
}

TEST(SftpProviderPath, ParentNormalisesBeforeSplitting) {
    SftpProvider p;
    EXPECT_EQ(p.parentPath("/srv/data/"), QString("/srv"));
    EXPECT_EQ(p.parentPath("/srv/./data/../x"), QString("/srv"));
}

// --- disconnected state -------------------------------------------------

TEST(SftpProviderState, FreshProviderIsNotConnected) {
    SftpProvider p;
    EXPECT_FALSE(p.isConnected());
    EXPECT_TRUE(p.host().isEmpty());
    // Off-line queries degrade gracefully rather than crashing.
    EXPECT_FALSE(p.exists("/anything"));
    EXPECT_FALSE(p.isDir("/anything"));
    EXPECT_TRUE(p.list("/anything", true).isEmpty());
}

// --- server-side move ---------------------------------------------------

TEST(SftpProviderMove, DisconnectedReportsUnsupportedNotFailure) {
    SftpProvider p;
    // Unsupported means "ask someone else"; Failed would make the transfer
    // engine treat a missing connection as a real error instead of falling
    // back to streaming.
    EXPECT_EQ(p.moveTo("/a/x.txt", "/b/x.txt"), FileProvider::RenameResult::Unsupported);
}

TEST(SftpProviderMove, RefusesMoveOntoItself) {
    SftpProvider p;
    // Same source and destination would otherwise risk a backend deleting the
    // only copy; rejected before any connection check.
    EXPECT_EQ(p.moveTo("/a/x.txt", "/a/./x.txt"), FileProvider::RenameResult::Failed);
}
