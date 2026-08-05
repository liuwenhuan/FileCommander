#include <gtest/gtest.h>

#include <QString>

#include "CurlFtpProvider.h"

// These tests exercise only the pure logic (cleanPath / parentPath and the
// MLSD/Unix listing parsers), which needs no live FTP server. Connect/list/
// stat/rename over the wire are not covered here, mirroring
// test_SftpProvider.cpp's approach for the sibling SFTP backend.

// --- cleanPath ------------------------------------------------------------

TEST(CurlFtpProviderTest, Path_CleanCollapsesRedundantSlashes) {
    CurlFtpProvider p;
    EXPECT_EQ(p.cleanPath("/srv//data///x"), QString("/srv/data/x"));
}

TEST(CurlFtpProviderTest, Path_CleanResolvesDotAndDotDot) {
    CurlFtpProvider p;
    EXPECT_EQ(p.cleanPath("/srv/./data/../logs"), QString("/srv/logs"));
}

TEST(CurlFtpProviderTest, Path_CleanEmptyFallsBackToRoot) {
    CurlFtpProvider p;
    EXPECT_EQ(p.cleanPath(""), QString("/"));
}

// --- parentPath -------------------------------------------------------------

TEST(CurlFtpProviderTest, Path_ParentOfNestedPath) {
    CurlFtpProvider p;
    EXPECT_EQ(p.parentPath("/srv/data/x"), QString("/srv/data"));
}

TEST(CurlFtpProviderTest, Path_ParentOfRootIsEmpty) {
    CurlFtpProvider p;
    EXPECT_EQ(p.parentPath("/"), QString());
}

// --- disconnected state -----------------------------------------------------

TEST(CurlFtpProviderTest, State_FreshProviderIsNotConnected) {
    CurlFtpProvider p;
    EXPECT_FALSE(p.isConnected());
    EXPECT_TRUE(p.host().isEmpty());
    EXPECT_FALSE(p.exists("/anything"));
    EXPECT_FALSE(p.isDir("/anything"));
    EXPECT_TRUE(p.list("/anything", true).isEmpty());
    EXPECT_FALSE(p.remove("/anything"));
    EXPECT_FALSE(p.mkdir("/anything"));
}

// --- parseMlsdListing (RFC 3659) --------------------------------------------

TEST(CurlFtpProviderTest, Mlsd_ParsesFilesAndDirectories) {
    const QByteArray data =
        "type=cdir;modify=20240101000000; .\r\n"
        "type=pdir;modify=20240101000000; ..\r\n"
        "type=dir;modify=20240102030405; sub\r\n"
        "type=file;size=1234;modify=20240103040506;unix.mode=0644; readme.txt\r\n";

    const QVector<FileInfo> entries = CurlFtpProvider::parseMlsdListing(data, "/srv", true);
    ASSERT_EQ(entries.size(), 2);

    EXPECT_EQ(entries[0].name(), QString("sub"));
    EXPECT_TRUE(entries[0].isDir());
    EXPECT_EQ(entries[0].path(), QString("/srv/sub"));

    EXPECT_EQ(entries[1].name(), QString("readme.txt"));
    EXPECT_FALSE(entries[1].isDir());
    EXPECT_EQ(entries[1].size(), 1234);
    EXPECT_EQ(entries[1].path(), QString("/srv/readme.txt"));
}

TEST(CurlFtpProviderTest, Mlsd_SkipsCdirPdirAndHiddenUnlessShown) {
    const QByteArray data =
        "type=cdir;modify=20240101000000; .\r\n"
        "type=file;size=1;modify=20240101000000; .hidden\r\n"
        "type=file;size=2;modify=20240101000000; visible\r\n";

    EXPECT_EQ(CurlFtpProvider::parseMlsdListing(data, "/srv", false).size(), 1);
    EXPECT_EQ(CurlFtpProvider::parseMlsdListing(data, "/srv", true).size(), 2);
}

TEST(CurlFtpProviderTest, Mlsd_EmptyListingProducesNoEntries) {
    EXPECT_TRUE(CurlFtpProvider::parseMlsdListing(QByteArray(), "/srv", true).isEmpty());
}

// --- parseUnixListing (classic LIST fallback) -------------------------------

TEST(CurlFtpProviderTest, UnixListing_ParsesFilesAndDirectories) {
    const QByteArray data =
        "drwxr-xr-x 2 user group    4096 Jan 02  2024 sub\r\n"
        "-rw-r--r-- 1 user group    1234 Jan 03  2024 readme.txt\r\n";

    const QVector<FileInfo> entries = CurlFtpProvider::parseUnixListing(data, "/srv", true);
    ASSERT_EQ(entries.size(), 2);

    EXPECT_EQ(entries[0].name(), QString("sub"));
    EXPECT_TRUE(entries[0].isDir());

    EXPECT_EQ(entries[1].name(), QString("readme.txt"));
    EXPECT_FALSE(entries[1].isDir());
    EXPECT_EQ(entries[1].size(), 1234);
}

TEST(CurlFtpProviderTest, UnixListing_IgnoresUnparsableLines) {
    const QByteArray data = "total 8\r\nnot a listing line\r\n";
    EXPECT_TRUE(CurlFtpProvider::parseUnixListing(data, "/srv", true).isEmpty());
}

TEST(CurlFtpProviderTest, UnixListing_HidesDotfilesUnlessShowHidden) {
    const QByteArray data =
        "-rw-r--r-- 1 user group 1 Jan 03  2024 .hidden\r\n"
        "-rw-r--r-- 1 user group 1 Jan 03  2024 visible\r\n";

    EXPECT_EQ(CurlFtpProvider::parseUnixListing(data, "/srv", false).size(), 1);
    EXPECT_EQ(CurlFtpProvider::parseUnixListing(data, "/srv", true).size(), 2);
}

TEST(CurlFtpProviderTest, UnixListing_StripsSymlinkArrowTarget) {
    const QByteArray data = "lrwxrwxrwx 1 user group 5 Jan 03  2024 link -> target\r\n";
    const QVector<FileInfo> entries = CurlFtpProvider::parseUnixListing(data, "/srv", true);
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].name(), QString("link"));
}

// --- server-side move -------------------------------------------------------

TEST(CurlFtpProviderTest, Move_DisconnectedReportsUnsupportedNotFailure) {
    CurlFtpProvider p;
    EXPECT_EQ(p.moveTo("/a/x.txt", "/b/x.txt"), FileProvider::RenameResult::Unsupported);
}

TEST(CurlFtpProviderTest, Move_RefusesMoveOntoItself) {
    // RNFR/RNTO onto the same path is meaningless and, given that the command
    // pair silently overwrites its target, not worth issuing.
    CurlFtpProvider p;
    EXPECT_EQ(p.moveTo("/a/x.txt", "/a/./x.txt"), FileProvider::RenameResult::Failed);
}
