#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>

#include "ArchiveProvider.h"

// An archive's virtual paths are rooted at "/", so an archive holding an
// "etc/passwd" entry browses as the path "/etc/passwd". Handing that to QFile
// opens the real local file -- which is exactly what secure wipe would have
// overwritten. isLocalFilesystem() is what stops it, and ArchiveProvider must
// answer false even though (unlike a network backend) it has no displayName to
// distinguish it from the local backend.

TEST(ArchiveProviderLocality, ArchivePathsAreNotLocalFilesystemPaths) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QString payload = QDir(tmp.path()).filePath("payload");
    ASSERT_TRUE(QDir().mkpath(QDir(payload).filePath("etc")));
    QFile f(QDir(payload).filePath("etc/passwd"));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("root:x:0:0:root:/root:/bin/sh\n");
    f.close();
    // A second top level, so the single-root strip (see ArchiveLayout) doesn't
    // fold "etc/" away and leave the entry browsing as "/passwd".
    QFile g(QDir(payload).filePath("notes.txt"));
    ASSERT_TRUE(g.open(QIODevice::WriteOnly));
    g.write("x\n");
    g.close();

    const QString zipPath = QDir(tmp.path()).filePath("a.zip");
    QProcess zip;
    zip.setWorkingDirectory(payload);
    zip.start("zip", {"-q", "-r", zipPath, "etc", "notes.txt"});
    if (!zip.waitForStarted() || !zip.waitForFinished() || zip.exitCode() != 0)
        GTEST_SKIP() << "zip(1) unavailable";

    QString error;
    ArchiveProvider provider(zipPath, &error);
    ASSERT_TRUE(provider.isValid()) << error.toStdString();

    EXPECT_FALSE(provider.isLocalFilesystem());
    // The entry really does browse under a path that names a live local file:
    // this is the case the flag exists to reject, not a hypothetical one.
    EXPECT_TRUE(provider.exists("/etc/passwd"));
    EXPECT_TRUE(QFile::exists("/etc/passwd"));
}
