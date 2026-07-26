#include <gtest/gtest.h>

#include "CurlFtpProvider.h"
#include "CurlWebDavProvider.h"
#include "GvfsMounter.h"
#include "SftpProvider.h"
#include "SmbProvider.h"

using P = GvfsMounter::Protocol;

namespace {

RemoteLocation smbAt(const QString &host, const QString &user) {
    RemoteLocation loc;
    loc.scheme = QStringLiteral("smb");
    loc.host = host;
    loc.user = user;
    loc.password = QStringLiteral("secret");
    loc.anonymous = user.isEmpty();
    return loc;
}

RemoteLocation davAt(const QString &host, int port, const QString &user, bool https = false) {
    RemoteLocation loc;
    loc.scheme = https ? QStringLiteral("davs") : QStringLiteral("dav");
    loc.host = host;
    loc.port = port;
    loc.user = user;
    loc.password = QStringLiteral("secret");
    loc.anonymous = user.isEmpty();
    return loc;
}

RemoteLocation sftpAt(const QString &host, int port, const QString &user) {
    RemoteLocation loc;
    loc.scheme = QStringLiteral("sftp");
    loc.host = host;
    loc.port = port;
    loc.user = user;
    loc.password = QStringLiteral("secret");
    loc.anonymous = user.isEmpty();
    return loc;
}

// Mount directory names in the exact shape a live session produced them under
// /run/user/<uid>/gvfs -- field names, field order, the lower-cased share, the
// percent-encoded prefix, and which entries carry a user= at all. Only the host
// and account names are substituted (documentation values, RFC 5737 / RFC 2606).
// Everything that matches a URI to a mount is checked against these rather than
// against a name this code made up.
const QStringList kRealMountDirs = {
    QStringLiteral("smb-share:server=192.0.2.10,share=home"),
    QStringLiteral("smb-share:server=192.0.2.10,share=download,user=alice"),
    QStringLiteral("dav:host=192.0.2.11,port=5006,ssl=false,prefix=%2Fdav"),
    QStringLiteral("sftp:host=sftp.example.com,user=carol"),
};

QStringList urisOf(const QVector<GvfsMounter::MountTarget> &targets) {
    QStringList out;
    for (const auto &t : targets)
        out << t.uri;
    return out;
}

} // namespace

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
    EXPECT_EQ(GvfsMounter::defaultPortForScheme("davs"), 443);
    EXPECT_EQ(GvfsMounter::defaultPortForScheme("DAV"), 80);
    EXPECT_EQ(GvfsMounter::defaultPortForScheme("nonsense"), 0);
}

// --- uriForPath ---------------------------------------------------------

TEST(GvfsMounterUriForPath, EmbedsUserAndSkipsDefaultPort) {
    EXPECT_EQ(GvfsMounter::uriForPath(sftpAt("h", 22, "bob"), "/srv/data"),
              QString("sftp://bob@h/srv/data"));
    EXPECT_EQ(GvfsMounter::uriForPath(sftpAt("h", 2222, "bob"), "/srv"),
              QString("sftp://bob@h:2222/srv"));
    EXPECT_EQ(GvfsMounter::uriForPath(sftpAt("h", 22, ""), "/"),
              QString("sftp://h/"));
}

TEST(GvfsMounterUriForPath, PercentEncodesSpacesAndNonAscii) {
    // A filename is not a URI. Handing gio a raw one works by luck at best, and
    // a space ends the argument as far as any URI parser is concerned.
    EXPECT_EQ(GvfsMounter::uriForPath(smbAt("nas", "bob"), "/share/My Docs/a b.txt"),
              QString("smb://bob@nas/share/My%20Docs/a%20b.txt"));
    EXPECT_EQ(GvfsMounter::uriForPath(davAt("h", 5006, "bob"), "/dav/下载"),
              QString("dav://bob@h:5006/dav/%E4%B8%8B%E8%BD%BD"));
}

TEST(GvfsMounterUriForPath, TreatsPercentInFilenameLiterally) {
    // "100%.txt" is a real filename, not a broken escape sequence.
    EXPECT_EQ(GvfsMounter::uriForPath(smbAt("nas", ""), "/share/100%.txt"),
              QString("smb://nas/share/100%25.txt"));
    // ...and "%2F" as literal text must not decode back into a path separator.
    EXPECT_EQ(GvfsMounter::uriForPath(smbAt("nas", ""), "/share/a%2Fb.txt"),
              QString("smb://nas/share/a%252Fb.txt"));
}

TEST(GvfsMounterUriForPath, InvalidLocationYieldsNothing) {
    EXPECT_TRUE(GvfsMounter::uriForPath(RemoteLocation(), "/x").isEmpty());
    RemoteLocation noHost;
    noHost.scheme = QStringLiteral("sftp");
    EXPECT_TRUE(GvfsMounter::uriForPath(noHost, "/x").isEmpty());
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

TEST(GvfsMounterDir, SftpAnonymousDropsUserField) {
    EXPECT_EQ(GvfsMounter::gvfsMountDirName("sftp://example.com/"),
              QString("sftp:host=example.com"));
}

TEST(GvfsMounterDir, SmbConvention) {
    EXPECT_EQ(GvfsMounter::gvfsMountDirName("smb://nas/media/pics"),
              QString("smb-share:server=nas,share=media"));
}

TEST(GvfsMounterDir, SmbLowerCasesServerAndShare) {
    // The share on the server really is spelled "Download"; gvfs mounts it as
    // "share=download" regardless. Constructing "share=Download" names a
    // directory that does not exist.
    EXPECT_EQ(GvfsMounter::gvfsMountDirName("smb://alice@NAS.local/Download/sub"),
              QString("smb-share:server=nas.local,share=download,user=alice"));
}

TEST(GvfsMounterDir, SmbWithoutShareNamesNoMount) {
    // smb://host/ is the list of shares, served by a different backend. There is
    // no file behind it, so there is no local path to offer.
    EXPECT_TRUE(GvfsMounter::gvfsMountDirName("smb://nas/").isEmpty());
}

TEST(GvfsMounterDir, DavSslFlag) {
    EXPECT_EQ(GvfsMounter::gvfsMountDirName("dav://h/"),
              QString("dav:host=h,ssl=false"));
    EXPECT_EQ(GvfsMounter::gvfsMountDirName("davs://h/"),
              QString("dav:host=h,ssl=true"));
}

TEST(GvfsMounterDir, DavCarriesPortAndPrefix) {
    // Verbatim from a live session: a WebDAV mount taken with a path records
    // that path as a percent-encoded prefix= field. Leaving it out named a
    // directory that never exists on any server not rooted at "/".
    EXPECT_EQ(GvfsMounter::gvfsMountDirName("dav://192.0.2.11:5006/dav"),
              QString("dav:host=192.0.2.11,port=5006,ssl=false,prefix=%2Fdav"));
    EXPECT_EQ(GvfsMounter::gvfsMountDirName("dav://bob@192.0.2.11:5006/dav"),
              QString("dav:host=192.0.2.11,port=5006,ssl=false,user=bob,prefix=%2Fdav"));
}

TEST(GvfsMounterDir, DavPrefixEncodesEverySeparator) {
    EXPECT_EQ(GvfsMounter::gvfsMountDirName("dav://h/a/b"),
              QString("dav:host=h,ssl=false,prefix=%2Fa%2Fb"));
}

TEST(GvfsMounterDir, InvalidUri) {
    EXPECT_TRUE(GvfsMounter::gvfsMountDirName("not a uri").isEmpty());
    EXPECT_TRUE(GvfsMounter::gvfsMountDirName("nfs://host/export").isEmpty());
}

// --- matchMountDir ------------------------------------------------------

TEST(GvfsMounterMatch, FindsRealSmbMountDespiteShareCase) {
    EXPECT_EQ(GvfsMounter::matchMountDir(kRealMountDirs, "smb://alice@192.0.2.10/Download"),
              QString("smb-share:server=192.0.2.10,share=download,user=alice"));
}

TEST(GvfsMounterMatch, FindsRealDavMountByPrefix) {
    EXPECT_EQ(GvfsMounter::matchMountDir(kRealMountDirs, "dav://192.0.2.11:5006/dav"),
              QString("dav:host=192.0.2.11,port=5006,ssl=false,prefix=%2Fdav"));
}

TEST(GvfsMounterMatch, FindsRealSftpMount) {
    EXPECT_EQ(GvfsMounter::matchMountDir(kRealMountDirs, "sftp://carol@sftp.example.com/home"),
              QString("sftp:host=sftp.example.com,user=carol"));
}

TEST(GvfsMounterMatch, DoesNotBorrowAMountWithADifferentLogin) {
    // "share=download,user=alice" is a session authenticated as alice. A URI
    // with no user, or another user, is a different session with possibly
    // different rights -- answering with it would read the share as someone else.
    EXPECT_TRUE(
        GvfsMounter::matchMountDir(kRealMountDirs, "smb://192.0.2.10/download").isEmpty());
    EXPECT_TRUE(
        GvfsMounter::matchMountDir(kRealMountDirs, "smb://other@192.0.2.10/download").isEmpty());
    // ...and the reverse: the userless "share=home" mount is not ours either.
    EXPECT_TRUE(
        GvfsMounter::matchMountDir(kRealMountDirs, "smb://alice@192.0.2.10/home").isEmpty());
}

TEST(GvfsMounterMatch, MatchesRegardlessOfFieldOrder) {
    const QStringList shuffled = {
        QStringLiteral("dav:prefix=%2Fdav,ssl=false,host=192.0.2.11,port=5006")};
    EXPECT_EQ(GvfsMounter::matchMountDir(shuffled, "dav://192.0.2.11:5006/dav"),
              shuffled.first());
}

TEST(GvfsMounterMatch, IgnoresEntriesThatAreNotMountSpecs) {
    const QStringList junk = {QStringLiteral("some-random-dir"), QStringLiteral(""),
                              QStringLiteral("dav:"), QStringLiteral("dav:novalue")};
    EXPECT_TRUE(GvfsMounter::matchMountDir(junk, "dav://192.0.2.11:5006/dav").isEmpty());
}

TEST(GvfsMounterMatch, WrongPrefixIsNotAMatch) {
    EXPECT_TRUE(
        GvfsMounter::matchMountDir(kRealMountDirs, "dav://192.0.2.11:5006/other").isEmpty());
    // A different port is a different server as far as gvfs is concerned.
    EXPECT_TRUE(
        GvfsMounter::matchMountDir(kRealMountDirs, "dav://192.0.2.11:80/dav").isEmpty());
}

// --- relativeUnder ------------------------------------------------------

TEST(GvfsMounterRelative, ServerRootPassesPathsThrough) {
    QString rel;
    ASSERT_TRUE(GvfsMounter::relativeUnder("/", "/home/carol/x.txt", &rel));
    EXPECT_EQ(rel, QString("home/carol/x.txt"));
    ASSERT_TRUE(GvfsMounter::relativeUnder("/", "/", &rel));
    EXPECT_EQ(rel, QString());
}

TEST(GvfsMounterRelative, SmbShareIsPartOfTheMountNotAPathUnderIt) {
    // "/download/a.7z" on an SMB provider is share "download" plus "a.7z"; the
    // share name must not survive into the path under the mount point.
    QString rel;
    ASSERT_TRUE(GvfsMounter::relativeUnder("/download", "/download/a.7z", &rel));
    EXPECT_EQ(rel, QString("a.7z"));
    ASSERT_TRUE(GvfsMounter::relativeUnder("/download", "/download", &rel));
    EXPECT_EQ(rel, QString());
}

TEST(GvfsMounterRelative, OnlySplitsAtSegmentBoundaries) {
    QString rel;
    EXPECT_FALSE(GvfsMounter::relativeUnder("/a", "/ab/c", &rel));
    EXPECT_FALSE(GvfsMounter::relativeUnder("/dav", "/davos", &rel));
    EXPECT_FALSE(GvfsMounter::relativeUnder("/a/b", "/a", &rel));
}

TEST(GvfsMounterRelative, ToleratesRedundantSlashes) {
    QString rel;
    ASSERT_TRUE(GvfsMounter::relativeUnder("/dav/", "//dav//sub///f.zip", &rel));
    EXPECT_EQ(rel, QString("sub/f.zip"));
}

TEST(GvfsMounterRelative, KeepsNonAsciiAndSpacesIntact) {
    QString rel;
    ASSERT_TRUE(GvfsMounter::relativeUnder("/dav", "/dav/下载/My Docs/文件 1.zip", &rel));
    EXPECT_EQ(rel, QString("下载/My Docs/文件 1.zip"));
}

// --- mountTargets -------------------------------------------------------

TEST(GvfsMounterTargets, SmbMountsTheShareNotTheServer) {
    const auto targets = GvfsMounter::mountTargets(smbAt("192.0.2.10", "alice"),
                                                   "/download/sub/a.7z");
    ASSERT_EQ(targets.size(), 2);
    EXPECT_EQ(targets[0].uri, QString("smb://alice@192.0.2.10/download"));
    EXPECT_EQ(targets[0].remoteRoot, QString("/download"));
    EXPECT_TRUE(targets[0].ownCredentials);
    // The same share as the desktop would have mounted it: recognised, never
    // created by us (gio's prompt order changes without a username in the URI).
    EXPECT_EQ(targets[1].uri, QString("smb://192.0.2.10/download"));
    EXPECT_FALSE(targets[1].ownCredentials);
}

TEST(GvfsMounterTargets, SmbServerRootHasNoMount) {
    EXPECT_TRUE(GvfsMounter::mountTargets(smbAt("nas", "bob"), "/").isEmpty());
    EXPECT_TRUE(GvfsMounter::mountTargets(smbAt("nas", "bob"), "").isEmpty());
}

TEST(GvfsMounterTargets, SftpAndFtpMountTheServerRoot) {
    const auto targets = GvfsMounter::mountTargets(sftpAt("h", 22, "carol"), "/home/carol/x");
    ASSERT_EQ(targets.size(), 2);
    EXPECT_EQ(targets[0].uri, QString("sftp://carol@h/"));
    EXPECT_EQ(targets[0].remoteRoot, QString("/"));
    EXPECT_EQ(targets[1].uri, QString("sftp://h/"));
}

TEST(GvfsMounterTargets, AnonymousHasNoSecondCandidate) {
    const auto targets = GvfsMounter::mountTargets(sftpAt("h", 22, ""), "/pub");
    ASSERT_EQ(targets.size(), 1);
    EXPECT_EQ(targets[0].uri, QString("sftp://h/"));
}

TEST(GvfsMounterTargets, WebDavWalksDownFromTheShallowestRoot) {
    // The server tested here refuses to be mounted at "/" ("not a WebDAV
    // share") but accepts "/dav". The connection cannot know that, so the
    // candidates go shallowest first and the first that mounts wins -- which
    // also means one mount ends up serving the whole connection.
    const auto uris =
        urisOf(GvfsMounter::mountTargets(davAt("192.0.2.11", 5006, "bob"), "/dav/sub/a.zip"));
    ASSERT_EQ(uris.size(), 8);
    EXPECT_EQ(uris[0], QString("dav://bob@192.0.2.11:5006/"));
    EXPECT_EQ(uris[1], QString("dav://192.0.2.11:5006/"));
    EXPECT_EQ(uris[2], QString("dav://bob@192.0.2.11:5006/dav"));
    EXPECT_EQ(uris[3], QString("dav://192.0.2.11:5006/dav"));
    EXPECT_EQ(uris[4], QString("dav://bob@192.0.2.11:5006/dav/sub"));
    EXPECT_EQ(uris[5], QString("dav://192.0.2.11:5006/dav/sub"));
    // Last resort, and only ever reached when everything shallower was refused:
    // the target itself, in case it is a directory that is the server's root.
    EXPECT_EQ(uris[6], QString("dav://bob@192.0.2.11:5006/dav/sub/a.zip"));
    EXPECT_EQ(uris[7], QString("dav://192.0.2.11:5006/dav/sub/a.zip"));
}

TEST(GvfsMounterTargets, WebDavRootsAreDeduplicated) {
    // "/dav/sub" is both the two-segment candidate and the target's parent; it
    // must not be probed (and so mounted) twice.
    const auto uris = urisOf(GvfsMounter::mountTargets(davAt("h", 0, ""), "/dav/sub/a.zip"));
    EXPECT_EQ(uris.size(), 4);
    EXPECT_EQ(uris.count(QString("dav://h/dav/sub")), 1);
}

TEST(GvfsMounterTargets, WebDavAddsDeepFallbacksForNestedRoots) {
    // A server whose root is several segments down still gets a candidate that
    // can actually be mounted: the target's own directory, then the target.
    const auto uris =
        urisOf(GvfsMounter::mountTargets(davAt("h", 0, ""), "/a/b/c/d/file.zip"));
    EXPECT_TRUE(uris.contains(QString("dav://h/a/b/c/d")));
    EXPECT_TRUE(uris.contains(QString("dav://h/a/b/c/d/file.zip")));
    // Default port stays out of the URI so it stays out of the mount spec.
    EXPECT_EQ(uris.first(), QString("dav://h/"));
}

TEST(GvfsMounterTargets, UnknownSchemeHasNoTargets) {
    RemoteLocation nfs;
    nfs.scheme = QStringLiteral("nfs");
    nfs.host = QStringLiteral("h");
    EXPECT_TRUE(GvfsMounter::mountTargets(nfs, "/export").isEmpty());
    EXPECT_TRUE(GvfsMounter::mountTargets(RemoteLocation(), "/x").isEmpty());
}

// --- mountAnswers -------------------------------------------------------
//
// The order is the whole point. `gio mount` re-prompts after a rejected answer,
// so feeding the lines in the wrong order does not merely fail: it sends the
// password to the server as a *username* first, one real failed login attempt
// per attempt. The sequences below were read off live `gio mount` runs against
// SMB, WebDAV and SFTP servers.

TEST(GvfsMounterAnswers, SmbWithUserInUriAnswersDomainThenPassword) {
    const auto answers =
        GvfsMounter::mountAnswers(smbAt("nas", "alice"), "smb://alice@nas/share");
    ASSERT_EQ(answers.size(), 2);
    EXPECT_EQ(answers[0], QString()); // Domain: empty accepts gio's default
    EXPECT_EQ(answers[1], QString("secret"));
}

TEST(GvfsMounterAnswers, SmbWithoutUserInUriAnswersUserDomainPassword) {
    const auto answers = GvfsMounter::mountAnswers(smbAt("nas", "alice"), "smb://nas/share");
    ASSERT_EQ(answers.size(), 3);
    EXPECT_EQ(answers[0], QString("alice"));
    EXPECT_EQ(answers[1], QString());
    EXPECT_EQ(answers[2], QString("secret"));
}

TEST(GvfsMounterAnswers, NonSmbHasNoDomainStep) {
    // WebDAV and SFTP never ask for a domain; an extra blank line there would
    // be read as the password.
    const auto dav = GvfsMounter::mountAnswers(davAt("h", 5006, "bob"), "dav://bob@h:5006/dav");
    ASSERT_EQ(dav.size(), 1);
    EXPECT_EQ(dav[0], QString("secret"));

    const auto sftp = GvfsMounter::mountAnswers(sftpAt("h", 22, "carol"), "sftp://h/");
    ASSERT_EQ(sftp.size(), 2);
    EXPECT_EQ(sftp[0], QString("carol"));
    EXPECT_EQ(sftp[1], QString("secret"));
}

TEST(GvfsMounterAnswers, AnonymousAnswersNothing) {
    RemoteLocation loc = smbAt("nas", QString());
    EXPECT_TRUE(GvfsMounter::mountAnswers(loc, "smb://nas/public").isEmpty());
}

// --- gioEnvironment -----------------------------------------------------

TEST(GvfsMounterEnv, ForcesEnglishMessagesWithoutTouchingTheEncoding) {
    // This is the locale bug in full: the answer is found by matching the label
    // "local path:", which a zh_CN session prints as "本地路径:". LC_ALL=C fixes
    // the label and then mangles every non-ASCII character of the path itself
    // to "?", so only the message locale may be pinned.
    QProcessEnvironment base;
    base.insert(QStringLiteral("LANG"), QStringLiteral("zh_CN.UTF-8"));
    base.insert(QStringLiteral("LC_ALL"), QStringLiteral("zh_CN.UTF-8"));
    base.insert(QStringLiteral("LANGUAGE"), QStringLiteral("zh_CN:zh"));

    const QProcessEnvironment env = GvfsMounter::gioEnvironment(base);
    EXPECT_FALSE(env.contains(QStringLiteral("LC_ALL")));
    EXPECT_EQ(env.value(QStringLiteral("LANGUAGE")), QString("C"));
    EXPECT_EQ(env.value(QStringLiteral("LC_MESSAGES")), QString("C"));
    EXPECT_EQ(env.value(QStringLiteral("LANG")), QString("zh_CN.UTF-8"));
    EXPECT_FALSE(env.contains(QStringLiteral("LC_CTYPE")));
}

TEST(GvfsMounterEnv, SuppliesAUtf8CtypeWhenTheSessionHasNone) {
    QProcessEnvironment bare;
    EXPECT_EQ(GvfsMounter::gioEnvironment(bare).value(QStringLiteral("LC_CTYPE")),
              QString("C.UTF-8"));

    QProcessEnvironment posix;
    posix.insert(QStringLiteral("LANG"), QStringLiteral("POSIX"));
    EXPECT_EQ(GvfsMounter::gioEnvironment(posix).value(QStringLiteral("LC_CTYPE")),
              QString("C.UTF-8"));
}

// --- parseLocalPath -----------------------------------------------------

TEST(GvfsMounterParse, PullsLocalPathOutOfGioInfo) {
    const QString out =
        "display name: 192.0.2.10 on home\n"
        "type: directory\n"
        "uri: smb://192.0.2.10/home/\n"
        "local path: /run/user/1000/gvfs/smb-share:server=192.0.2.10,share=home\n"
        "unix mount: gvfsd-fuse /run/user/1000/gvfs fuse.gvfsd-fuse rw\n";
    EXPECT_EQ(GvfsMounter::parseLocalPath(out),
              QString("/run/user/1000/gvfs/smb-share:server=192.0.2.10,share=home"));
}

TEST(GvfsMounterParse, LocalPathKeepsNonAsciiAndSpaces) {
    const QString out = "local path: /run/user/1000/gvfs/dav:host=h,ssl=false,prefix=%2Fdav/下载/a b.zip\n";
    EXPECT_EQ(GvfsMounter::parseLocalPath(out),
              QString("/run/user/1000/gvfs/dav:host=h,ssl=false,prefix=%2Fdav/下载/a b.zip"));
}

TEST(GvfsMounterParse, NoLocalPathForANonLocalUri) {
    EXPECT_TRUE(GvfsMounter::parseLocalPath("uri: sftp://h/\ntype: directory\n").isEmpty());
    EXPECT_TRUE(GvfsMounter::parseLocalPath(QString()).isEmpty());
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

// --- localPathFor refusals ----------------------------------------------
//
// The rest of localPathFor() talks to a live gvfs daemon and is covered by the
// real-server runs; what must hold with no server anywhere is that it refuses
// rather than inventing a path, because every caller is about to hand the
// result to something that opens files by name.

TEST(GvfsMounterResolve, RefusesWithoutAProvider) {
    EXPECT_TRUE(GvfsMounter::localPathFor(nullptr, "/x").isEmpty());
}

TEST(GvfsMounterResolve, RefusesAnInvalidLocation) {
    EXPECT_TRUE(GvfsMounter::localPathFor(RemoteLocation(), "/x").isEmpty());
    EXPECT_FALSE(GvfsMounter::isMounted(RemoteLocation(), "/x"));
}

TEST(GvfsMounterResolve, UnconnectedNetworkProvidersHaveNoLocation) {
    // A provider that never connected has no server to name, so it must not
    // produce a half-built URI that would then be mounted or matched.
    SftpProvider sftp;
    EXPECT_FALSE(sftp.remoteLocation().isValid());
    SmbProvider smb;
    EXPECT_FALSE(smb.remoteLocation().isValid());
    CurlFtpProvider ftp;
    EXPECT_FALSE(ftp.remoteLocation().isValid());
    CurlWebDavProvider dav;
    EXPECT_FALSE(dav.remoteLocation().isValid());

    EXPECT_TRUE(GvfsMounter::localPathFor(&sftp, "/home/carol").isEmpty());
    EXPECT_TRUE(GvfsMounter::localPathFor(&smb, "/share/f").isEmpty());
}

namespace {
// Stands in for a backend that is not a network connection at all.
class MinimalProvider : public FileProvider {
public:
    QVector<FileInfo> list(const QString &, bool) const override { return {}; }
    bool isDir(const QString &) const override { return false; }
    QString cleanPath(const QString &p) const override { return p; }
    QString parentPath(const QString &) const override { return {}; }
    bool exists(const QString &) const override { return false; }
    RenameResult rename(const QString &, const QString &, QString *) override {
        return RenameResult::Failed;
    }
};
} // namespace

TEST(GvfsMounterResolve, DefaultBackendOptsOut) {
    MinimalProvider p;
    EXPECT_FALSE(p.remoteLocation().isValid());
    EXPECT_TRUE(GvfsMounter::localPathFor(&p, "/etc/passwd").isEmpty());
}
