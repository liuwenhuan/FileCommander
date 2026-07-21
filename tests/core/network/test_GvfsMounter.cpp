#include <gtest/gtest.h>

#include "GvfsMounter.h"

using P = GvfsMounter::Protocol;

// --- buildUri -----------------------------------------------------------

TEST(GvfsMounterUri, SftpWithUserOmitsDefaultPort) {
    EXPECT_EQ(GvfsMounter::buildUri(P::Sftp, "example.com", 22, "bob", "/srv"),
              QString("sftp://bob@example.com/srv"));
}

TEST(GvfsMounterUri, SftpKeepsNonDefaultPort) {
    EXPECT_EQ(GvfsMounter::buildUri(P::Sftp, "host", 2222, "bob", "/data"),
              QString("sftp://bob@host:2222/data"));
}

TEST(GvfsMounterUri, SftpAnonymousOmitsUser) {
    EXPECT_EQ(GvfsMounter::buildUri(P::Sftp, "host", 22, "", "/"),
              QString("sftp://host/"));
}

TEST(GvfsMounterUri, PathIsNormalisedToSingleLeadingSlash) {
    EXPECT_EQ(GvfsMounter::buildUri(P::Sftp, "host", 22, "", "srv//data"),
              QString("sftp://host/srv//data"));
    EXPECT_EQ(GvfsMounter::buildUri(P::Sftp, "host", 22, "", ""),
              QString("sftp://host/"));
}

TEST(GvfsMounterUri, SmbUsesShareLayoutNoPort) {
    EXPECT_EQ(GvfsMounter::buildUri(P::Smb, "nas", 445, "", "media"),
              QString("smb://nas/media"));
    EXPECT_EQ(GvfsMounter::buildUri(P::Smb, "nas", 445, "alice", "media/pics"),
              QString("smb://alice@nas/media/pics"));
}

TEST(GvfsMounterUri, WebDavHttpVsHttps) {
    EXPECT_EQ(GvfsMounter::buildUri(P::WebDav, "dav.example.com", 80, "", "/files"),
              QString("dav://dav.example.com/files"));
    EXPECT_EQ(GvfsMounter::buildUri(P::WebDavs, "dav.example.com", 443, "", "/files"),
              QString("davs://dav.example.com/files"));
    EXPECT_EQ(GvfsMounter::buildUri(P::WebDav, "dav.example.com", 8080, "", "/files"),
              QString("dav://dav.example.com:8080/files"));
}

TEST(GvfsMounterUri, FtpDefaults) {
    EXPECT_EQ(GvfsMounter::buildUri(P::Ftp, "ftp.host", 21, "", "/pub"),
              QString("ftp://ftp.host/pub"));
    EXPECT_EQ(GvfsMounter::buildUri(P::Ftp, "ftp.host", 2121, "u", "/pub"),
              QString("ftp://u@ftp.host:2121/pub"));
}

TEST(GvfsMounterUri, EmptyHostYieldsEmptyUri) {
    EXPECT_TRUE(GvfsMounter::buildUri(P::Sftp, "  ", 22, "bob", "/").isEmpty());
}

TEST(GvfsMounterUri, DefaultPortsAndSchemes) {
    EXPECT_EQ(GvfsMounter::defaultPort(P::Sftp), 22);
    EXPECT_EQ(GvfsMounter::defaultPort(P::Smb), 445);
    EXPECT_EQ(GvfsMounter::defaultPort(P::WebDav), 80);
    EXPECT_EQ(GvfsMounter::defaultPort(P::WebDavs), 443);
    EXPECT_EQ(GvfsMounter::defaultPort(P::Ftp), 21);
    EXPECT_EQ(GvfsMounter::scheme(P::WebDavs), QString("davs"));
}

// --- gvfsMountDirName ---------------------------------------------------

TEST(GvfsMounterDir, SftpConvention) {
    EXPECT_EQ(GvfsMounter::gvfsMountDirName("sftp://bob@example.com/srv"),
              QString("sftp:host=example.com,user=bob"));
}

TEST(GvfsMounterDir, SftpWithPort) {
    EXPECT_EQ(GvfsMounter::gvfsMountDirName("sftp://bob@example.com:2222/srv"),
              QString("sftp:host=example.com,port=2222,user=bob"));
}

TEST(GvfsMounterDir, SmbConvention) {
    EXPECT_EQ(GvfsMounter::gvfsMountDirName("smb://nas/media/pics"),
              QString("smb-share:server=nas,share=media"));
}

TEST(GvfsMounterDir, DavSslFlag) {
    EXPECT_EQ(GvfsMounter::gvfsMountDirName("dav://h/x"),
              QString("dav:host=h,ssl=false"));
    EXPECT_EQ(GvfsMounter::gvfsMountDirName("davs://h/x"),
              QString("dav:host=h,ssl=true"));
}

TEST(GvfsMounterDir, InvalidUri) {
    EXPECT_TRUE(GvfsMounter::gvfsMountDirName("not a uri").isEmpty());
}

// --- parseMountList -----------------------------------------------------

TEST(GvfsMounterParse, ParsesMountLines) {
    const QString out =
        "Drive(0): ...\n"
        "Volume(0): whatever\n"
        "Mount(0): SFTP for bob on example.com -> sftp:host=example.com,user=bob/\n"
        "    Type: GProxyShadowMount\n"
        "Mount(1): media on nas -> smb://nas/media/\n";
    const auto mounts = GvfsMounter::parseMountList(out);
    ASSERT_EQ(mounts.size(), 2);
    EXPECT_EQ(mounts[0].name, QString("SFTP for bob on example.com"));
    EXPECT_EQ(mounts[0].uri, QString("sftp:host=example.com,user=bob/"));
    EXPECT_EQ(mounts[1].name, QString("media on nas"));
    EXPECT_EQ(mounts[1].uri, QString("smb://nas/media/"));
}

TEST(GvfsMounterParse, EmptyOutputNoMounts) {
    EXPECT_TRUE(GvfsMounter::parseMountList(QString()).isEmpty());
}

// --- parseNetworkList ---------------------------------------------------

TEST(GvfsMounterParse, ParsesNetworkLocations) {
    const QString out =
        "WORKGROUP\tsmb://workgroup/\n"
        "nas-server    smb://nas/\n";
    const auto locs = GvfsMounter::parseNetworkList(out);
    ASSERT_EQ(locs.size(), 2);
    EXPECT_EQ(locs[0].displayName, QString("WORKGROUP"));
    EXPECT_EQ(locs[0].uri, QString("smb://workgroup/"));
    EXPECT_EQ(locs[1].uri, QString("smb://nas/"));
}

TEST(GvfsMounterParse, NetworkListSkipsBlankLines) {
    EXPECT_TRUE(GvfsMounter::parseNetworkList("\n\n  \n").isEmpty());
}
