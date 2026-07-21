#include <gtest/gtest.h>

#include <QString>

#include "CurlFtpProvider.h"

// These tests exercise only the pure logic (cleanPath / parentPath and the
// MLSD/Unix listing parsers), which needs no live FTP server. Connect/list/
// stat/rename over the wire are not covered here, mirroring
// test_SftpProvider.cpp's approach for the sibling SFTP backend.

// --- cleanPath ------------------------------------------------------------

TEST(CurlFtpProviderPath, CleanCollapsesRedundantSlashes) {
    CurlFtpProvider p;
    EXPECT_EQ(p.cleanPath("/srv//data///x"), QString("/srv/data/x"));
}

TEST(CurlFtpProviderPath, CleanResolvesDotAndDotDot) {
    CurlFtpProvider p;
    EXPECT_EQ(p.cleanPath("/srv/./data/../logs"), QString("/srv/logs"));
}

TEST(CurlFtpProviderPath, CleanEmptyFallsBackToRoot) {
    CurlFtpProvider p;
    EXPECT_EQ(p.cleanPath(""), QString("/"));
}

// --- parentPath -------------------------------------------------------------

TEST(CurlFtpProviderPath, ParentOfNestedPath) {
    CurlFtpProvider p;
    EXPECT_EQ(p.parentPath("/srv/data/x"), QString("/srv/data"));
}

TEST(CurlFtpProviderPath, ParentOfRootIsEmpty) {
    CurlFtpProvider p;
    EXPECT_EQ(p.parentPath("/"), QString());
}

// --- disconnected state -----------------------------------------------------

TEST(CurlFtpProviderState, FreshProviderIsNotConnected) {
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

TEST(CurlFtpProviderMlsd, ParsesFilesAndDirectories) {
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

TEST(CurlFtpProviderMlsd, SkipsCdirPdirAndHiddenUnlessShown) {
    const QByteArray data =
        "type=cdir;modify=20240101000000; .\r\n"
        "type=file;size=1;modify=20240101000000; .hidden\r\n"
        "type=file;size=2;modify=20240101000000; visible\r\n";

    EXPECT_EQ(CurlFtpProvider::parseMlsdListing(data, "/srv", false).size(), 1);
    EXPECT_EQ(CurlFtpProvider::parseMlsdListing(data, "/srv", true).size(), 2);
}

TEST(CurlFtpProviderMlsd, EmptyListingProducesNoEntries) {
    EXPECT_TRUE(CurlFtpProvider::parseMlsdListing(QByteArray(), "/srv", true).isEmpty());
}

// --- parseUnixListing (classic LIST fallback) -------------------------------

TEST(CurlFtpProviderUnixListing, ParsesFilesAndDirectories) {
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

TEST(CurlFtpProviderUnixListing, IgnoresUnparsableLines) {
    const QByteArray data = "total 8\r\nnot a listing line\r\n";
    EXPECT_TRUE(CurlFtpProvider::parseUnixListing(data, "/srv", true).isEmpty());
}

TEST(CurlFtpProviderUnixListing, HidesDotfilesUnlessShowHidden) {
    const QByteArray data =
        "-rw-r--r-- 1 user group 1 Jan 03  2024 .hidden\r\n"
        "-rw-r--r-- 1 user group 1 Jan 03  2024 visible\r\n";

    EXPECT_EQ(CurlFtpProvider::parseUnixListing(data, "/srv", false).size(), 1);
    EXPECT_EQ(CurlFtpProvider::parseUnixListing(data, "/srv", true).size(), 2);
}

TEST(CurlFtpProviderUnixListing, StripsSymlinkArrowTarget) {
    const QByteArray data = "lrwxrwxrwx 1 user group 5 Jan 03  2024 link -> target\r\n";
    const QVector<FileInfo> entries = CurlFtpProvider::parseUnixListing(data, "/srv", true);
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].name(), QString("link"));
}
