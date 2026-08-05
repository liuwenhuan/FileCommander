#include <gtest/gtest.h>

#include <QString>

#include "CurlWebDavProvider.h"

// These tests exercise only the pure logic (cleanPath / parentPath and the
// PROPFIND multistatus XML parser), which needs no live WebDAV server.
// Connect/list/stat/rename over the wire are not covered here, mirroring
// test_SftpProvider.cpp's approach for the sibling SFTP backend.

// --- cleanPath / parentPath -------------------------------------------------

TEST(CurlWebDavProviderTest, Path_CleanCollapsesRedundantSlashes) {
    CurlWebDavProvider p;
    EXPECT_EQ(p.cleanPath("/srv//data///x"), QString("/srv/data/x"));
}

TEST(CurlWebDavProviderTest, Path_ParentOfNestedPath) {
    CurlWebDavProvider p;
    EXPECT_EQ(p.parentPath("/srv/data/x"), QString("/srv/data"));
}

TEST(CurlWebDavProviderTest, Path_ParentOfRootIsEmpty) {
    CurlWebDavProvider p;
    EXPECT_EQ(p.parentPath("/"), QString());
}

#ifdef Q_OS_WIN
TEST(CurlWebDavProviderTest, Path_ConvertsHttpsUrlToWindowsWebDavUnc) {
    const QString unc = CurlWebDavProvider::webDavUrlToUncForShell(
        QStringLiteral("https://example.test:9443/dav/movie.mp4"));

    EXPECT_TRUE(unc.startsWith(QStringLiteral("\\\\")));
    EXPECT_TRUE(unc.contains(QStringLiteral("example.test")));
    EXPECT_TRUE(unc.contains(QStringLiteral("movie.mp4")));
}
#endif

// --- disconnected state -----------------------------------------------------

TEST(CurlWebDavProviderTest, State_FreshProviderIsNotConnected) {
    CurlWebDavProvider p;
    EXPECT_FALSE(p.isConnected());
    EXPECT_TRUE(p.host().isEmpty());
    EXPECT_FALSE(p.exists("/anything"));
    EXPECT_FALSE(p.isDir("/anything"));
    EXPECT_TRUE(p.list("/anything", true).isEmpty());
    EXPECT_FALSE(p.remove("/anything"));
    EXPECT_FALSE(p.mkdir("/anything"));
}

// A seek() to a nonzero offset on a write handle must be refused: WebDAV PUT
// has no standardised resume mechanism, so a real seek there would risk
// silently uploading a truncated file. There is no open handle here (no live
// connection), so openWrite() itself already returns nullptr; the documented
// behaviour is exercised directly against a null handle to confirm seek()
// never reports success without one.
TEST(CurlWebDavProviderTest, State_SeekOnNullHandleFails) {
    CurlWebDavProvider p;
    EXPECT_FALSE(p.seek(nullptr, 0));
    EXPECT_FALSE(p.seek(nullptr, 100));
}

// --- parsePropfindXml --------------------------------------------------------

namespace {
const QByteArray kMultistatus = QByteArrayLiteral(
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<D:multistatus xmlns:D=\"DAV:\">"
    "  <D:response>"
    "    <D:href>/srv/</D:href>"
    "    <D:propstat>"
    "      <D:prop>"
    "        <D:resourcetype><D:collection/></D:resourcetype>"
    "        <D:getlastmodified>Wed, 03 Jan 2024 04:05:06 GMT</D:getlastmodified>"
    "      </D:prop>"
    "      <D:status>HTTP/1.1 200 OK</D:status>"
    "    </D:propstat>"
    "  </D:response>"
    "  <D:response>"
    "    <D:href>/srv/sub/</D:href>"
    "    <D:propstat>"
    "      <D:prop>"
    "        <D:resourcetype><D:collection/></D:resourcetype>"
    "        <D:getlastmodified>Wed, 03 Jan 2024 04:05:06 GMT</D:getlastmodified>"
    "      </D:prop>"
    "      <D:status>HTTP/1.1 200 OK</D:status>"
    "    </D:propstat>"
    "  </D:response>"
    "  <D:response>"
    "    <D:href>/srv/readme.txt</D:href>"
    "    <D:propstat>"
    "      <D:prop>"
    "        <D:resourcetype/>"
    "        <D:getcontentlength>1234</D:getcontentlength>"
    "        <D:getlastmodified>Wed, 03 Jan 2024 04:05:06 GMT</D:getlastmodified>"
    "      </D:prop>"
    "      <D:status>HTTP/1.1 200 OK</D:status>"
    "    </D:propstat>"
    "  </D:response>"
    "  <D:response>"
    "    <D:href>/srv/.hidden</D:href>"
    "    <D:propstat>"
    "      <D:prop>"
    "        <D:resourcetype/>"
    "        <D:getcontentlength>1</D:getcontentlength>"
    "      </D:prop>"
    "      <D:status>HTTP/1.1 200 OK</D:status>"
    "    </D:propstat>"
    "  </D:response>"
    "</D:multistatus>");
} // namespace

TEST(CurlWebDavProviderTest, Propfind_SkipsSelfEntryAndParsesRest) {
    const QVector<FileInfo> entries =
        CurlWebDavProvider::parsePropfindXml(kMultistatus, "/srv", true);
    // 3 entries: sub (dir), readme.txt (file), .hidden (file) — the "/srv/"
    // self-response is excluded because it matches basePath.
    ASSERT_EQ(entries.size(), 3);

    EXPECT_EQ(entries[0].name(), QString("sub"));
    EXPECT_TRUE(entries[0].isDir());
    EXPECT_EQ(entries[0].path(), QString("/srv/sub"));

    EXPECT_EQ(entries[1].name(), QString("readme.txt"));
    EXPECT_FALSE(entries[1].isDir());
    EXPECT_EQ(entries[1].size(), 1234);

    EXPECT_EQ(entries[2].name(), QString(".hidden"));
}

TEST(CurlWebDavProviderTest, Propfind_HidesDotfilesUnlessShowHidden) {
    const QVector<FileInfo> entries =
        CurlWebDavProvider::parsePropfindXml(kMultistatus, "/srv", false);
    ASSERT_EQ(entries.size(), 2);
    for (const FileInfo &e : entries)
        EXPECT_FALSE(e.name().startsWith('.'));
}

TEST(CurlWebDavProviderTest, Propfind_EmptyBasePathKeepsEveryEntryIncludingSelf) {
    // As documented: passing a basePath that can never match a real href (a
    // null/empty QString here) keeps every <response>, including the
    // collection's own entry — used internally for a Depth:0 single-entry stat.
    const QVector<FileInfo> entries =
        CurlWebDavProvider::parsePropfindXml(kMultistatus, QString(), true);
    EXPECT_EQ(entries.size(), 4);
}

TEST(CurlWebDavProviderTest, Propfind_MalformedXmlProducesNoEntries) {
    EXPECT_TRUE(CurlWebDavProvider::parsePropfindXml("not xml", "/srv", true).isEmpty());
    EXPECT_TRUE(CurlWebDavProvider::parsePropfindXml(QByteArray(), "/srv", true).isEmpty());
}

// --- server-side move ---------------------------------------------------

TEST(CurlWebDavProviderTest, Move_DisconnectedReportsUnsupportedNotFailure) {
    CurlWebDavProvider p;
    EXPECT_EQ(p.moveTo("/a/x.txt", "/b/x.txt"), FileProvider::RenameResult::Unsupported);
}

TEST(CurlWebDavProviderTest, Move_RefusesMoveOntoItself) {
    CurlWebDavProvider p;
    EXPECT_EQ(p.moveTo("/a/x.txt", "/a/./x.txt"), FileProvider::RenameResult::Failed);
}
