#include <gtest/gtest.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include "FileOperations.h"
#include "FileProvider.h"
#include "LocalFileProvider.h"

// A copy must carry its source's modification time. Without it the destination
// is dated to the moment it was written, so a freshly copied file looks NEWER
// than the original it came from -- which made "Synchronize Directories" report
// every just-copied file as a difference again, with the arrow pointing back
// the way it came, and a second sync would overwrite the source from the copy.
namespace {

const qint64 kOldEpochSecs = 1709254800; // 2024-03-01 09:00:00 UTC+8

QString writeFile(const QString &dir, const QString &name, const QByteArray &content) {
    const QString path = QDir(dir).filePath(name);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write(content);
    file.close();
    return path;
}

// Backdates a file well into the past, so "preserved" is unmistakably distinct
// from "stamped just now".
void backdate(const QString &path, qint64 epochSecs = kOldEpochSecs) {
    QFile file(path);
    file.open(QIODevice::ReadWrite);
    file.setFileTime(QDateTime::fromSecsSinceEpoch(epochSecs),
                     QFileDevice::FileModificationTime);
    file.close();
}

QDateTime mtimeOf(const QString &path) {
    return QFileInfo(path).lastModified();
}

// Stands in for a backend that cannot set times (WebDAV, and every backend
// whose support hasn't been verified against a real server). Everything else
// delegates to the real local provider, so a copy through it genuinely works.
class NoTimeSupportProvider : public FileProvider {
public:
    QVector<FileInfo> list(const QString &path, bool showHidden) const override {
        return LocalFileProvider::instance()->list(path, showHidden);
    }
    bool isDir(const QString &path) const override {
        return LocalFileProvider::instance()->isDir(path);
    }
    QString cleanPath(const QString &p) const override {
        return LocalFileProvider::instance()->cleanPath(p);
    }
    QString parentPath(const QString &p) const override {
        return LocalFileProvider::instance()->parentPath(p);
    }
    bool exists(const QString &path) const override {
        return LocalFileProvider::instance()->exists(path);
    }
    RenameResult rename(const QString &path, const QString &newName, QString *newPath) override {
        return LocalFileProvider::instance()->rename(path, newName, newPath);
    }
    FileHandle *openRead(const QString &path) override {
        return LocalFileProvider::instance()->openRead(path);
    }
    FileHandle *openWrite(const QString &path, bool truncate) override {
        return LocalFileProvider::instance()->openWrite(path, truncate);
    }
    qint64 read(FileHandle *h, char *buf, qint64 max) override {
        return LocalFileProvider::instance()->read(h, buf, max);
    }
    qint64 write(FileHandle *h, const char *buf, qint64 size) override {
        return LocalFileProvider::instance()->write(h, buf, size);
    }
    bool seek(FileHandle *h, qint64 off) override {
        return LocalFileProvider::instance()->seek(h, off);
    }
    qint64 handleSize(FileHandle *h) override {
        return LocalFileProvider::instance()->handleSize(h);
    }
    void closeHandle(FileHandle *h) override { LocalFileProvider::instance()->closeHandle(h); }
    bool canStream() const override { return true; }
    bool remove(const QString &path) override {
        return LocalFileProvider::instance()->remove(path);
    }
    bool mkdir(const QString &path) override {
        return LocalFileProvider::instance()->mkdir(path);
    }

    // The point of this stub: the backend refuses to set times.
    bool setModifiedTime(const QString &, const QDateTime &) override {
        ++m_setAttempts;
        return false;
    }
    int setAttempts() const { return m_setAttempts; }

private:
    int m_setAttempts = 0;
};

// Delegates to the local provider but counts list() calls, so a test can pin
// how many directory round trips a transfer actually costs.
class CountingProvider : public FileProvider {
public:
    QVector<FileInfo> list(const QString &path, bool showHidden) const override {
        ++m_listCalls;
        return LocalFileProvider::instance()->list(path, showHidden);
    }
    bool isDir(const QString &path) const override {
        return LocalFileProvider::instance()->isDir(path);
    }
    QString cleanPath(const QString &p) const override {
        return LocalFileProvider::instance()->cleanPath(p);
    }
    QString parentPath(const QString &p) const override {
        return LocalFileProvider::instance()->parentPath(p);
    }
    bool exists(const QString &path) const override {
        return LocalFileProvider::instance()->exists(path);
    }
    RenameResult rename(const QString &path, const QString &newName, QString *newPath) override {
        return LocalFileProvider::instance()->rename(path, newName, newPath);
    }
    FileHandle *openRead(const QString &path) override {
        return LocalFileProvider::instance()->openRead(path);
    }
    FileHandle *openWrite(const QString &path, bool truncate) override {
        return LocalFileProvider::instance()->openWrite(path, truncate);
    }
    qint64 read(FileHandle *h, char *buf, qint64 max) override {
        return LocalFileProvider::instance()->read(h, buf, max);
    }
    qint64 write(FileHandle *h, const char *buf, qint64 size) override {
        return LocalFileProvider::instance()->write(h, buf, size);
    }
    bool seek(FileHandle *h, qint64 off) override {
        return LocalFileProvider::instance()->seek(h, off);
    }
    qint64 handleSize(FileHandle *h) override {
        return LocalFileProvider::instance()->handleSize(h);
    }
    void closeHandle(FileHandle *h) override { LocalFileProvider::instance()->closeHandle(h); }
    bool canStream() const override { return true; }
    bool remove(const QString &path) override {
        return LocalFileProvider::instance()->remove(path);
    }
    bool mkdir(const QString &path) override {
        return LocalFileProvider::instance()->mkdir(path);
    }
    bool setModifiedTime(const QString &path, const QDateTime &modified) override {
        return LocalFileProvider::instance()->setModifiedTime(path, modified);
    }

    int listCalls() const { return m_listCalls; }

private:
    mutable int m_listCalls = 0;
};

} // namespace

TEST(PreserveMtimeTest, LocalCopyKeepsSourceTime) {
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const QString source = writeFile(srcDir.path(), "doc.txt", "hello");
    backdate(source);
    const QDateTime want = mtimeOf(source);

    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.copyPaths({source}, dstDir.path(), nullptr, &err)) << err.toStdString();

    const QString target = QDir(dstDir.path()).filePath("doc.txt");
    ASSERT_TRUE(QFile::exists(target));
    EXPECT_EQ(mtimeOf(target), want);
}

TEST(PreserveMtimeTest, RecursiveCopyKeepsEachFileTime) {
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const QString treeRoot = QDir(srcDir.path()).filePath("tree");
    const QString a = writeFile(treeRoot, "a.txt", "a");
    const QString b = writeFile(treeRoot + "/nested", "b.txt", "b");
    backdate(a, kOldEpochSecs);
    backdate(b, kOldEpochSecs + 3600);
    const QDateTime wantA = mtimeOf(a);
    const QDateTime wantB = mtimeOf(b);

    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.copyPaths({treeRoot}, dstDir.path(), nullptr, &err)) << err.toStdString();

    const QDir out(QDir(dstDir.path()).filePath("tree"));
    EXPECT_EQ(mtimeOf(out.filePath("a.txt")), wantA);
    EXPECT_EQ(mtimeOf(out.filePath("nested/b.txt")), wantB);
}

TEST(PreserveMtimeTest, CopyAsKeepsSourceTime) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString source = writeFile(dir.path(), "orig.txt", "content");
    backdate(source);
    const QDateTime want = mtimeOf(source);

    const QString target = QDir(dir.path()).filePath("renamed.txt");
    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.copyAs(source, target, nullptr, &err)) << err.toStdString();

    EXPECT_EQ(mtimeOf(target), want);
}

TEST(PreserveMtimeTest, MoveKeepsSourceTime) {
    // A move across filesystems is a copy followed by a delete, so it runs the
    // same primitive and must preserve the time too.
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const QString source = writeFile(srcDir.path(), "moved.txt", "payload");
    backdate(source);
    const QDateTime want = mtimeOf(source);

    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.movePaths({source}, dstDir.path(), nullptr, &err)) << err.toStdString();

    const QString target = QDir(dstDir.path()).filePath("moved.txt");
    ASSERT_TRUE(QFile::exists(target));
    EXPECT_FALSE(QFile::exists(source));
    EXPECT_EQ(mtimeOf(target), want);
}

TEST(PreserveMtimeTest, SyncIsIdempotentAfterCopy) {
    // The end-to-end judgement: copy a file, then re-stat both sides the way the
    // comparison does. Before mtime preservation the destination came out ~years
    // newer than its own source, so the pair was classified as a difference --
    // and the newer side was the copy, which is what reversed the arrow.
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const QString source = writeFile(srcDir.path(), "doc.txt", "hello");
    backdate(source);

    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.copyPaths({source}, dstDir.path(), nullptr, &err)) << err.toStdString();

    const QString target = QDir(dstDir.path()).filePath("doc.txt");
    const qint64 deltaMs = mtimeOf(source).msecsTo(mtimeOf(target));
    EXPECT_LE(qAbs(deltaMs), 2000) << "copy drifted " << deltaMs << " ms from its source";
    EXPECT_EQ(QFileInfo(source).size(), QFileInfo(target).size());
}

TEST(PreserveMtimeTest, CrossProviderTransferKeepsSourceTime) {
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const QString source = writeFile(srcDir.path(), "stream.bin", QByteArray(4096, 'z'));
    backdate(source);
    const QDateTime want = mtimeOf(source);

    auto *provider = LocalFileProvider::instance();
    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.copyAcrossProviders(provider, {source}, provider, dstDir.path(),
                                         /*removeSource=*/false, nullptr, &err))
        << err.toStdString();

    EXPECT_EQ(mtimeOf(QDir(dstDir.path()).filePath("stream.bin")), want);
}

TEST(PreserveMtimeTest, UnsupportedBackendStillReportsCopySuccess) {
    // The hard requirement: a backend that cannot set times must degrade to the
    // old behaviour (a fresh timestamp), NOT make the user think the copy failed.
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const QByteArray payload(2048, 'q');
    const QString source = writeFile(srcDir.path(), "nostamp.bin", payload);
    backdate(source);

    NoTimeSupportProvider dst;
    FileOperations ops;
    QString err;
    EXPECT_TRUE(ops.copyAcrossProviders(LocalFileProvider::instance(), {source}, &dst,
                                         dstDir.path(), /*removeSource=*/false, nullptr, &err))
        << err.toStdString();

    // The file really did land, in full, despite the refused restamp.
    const QString target = QDir(dstDir.path()).filePath("nostamp.bin");
    ASSERT_TRUE(QFile::exists(target));
    EXPECT_EQ(QFileInfo(target).size(), payload.size());
    EXPECT_GT(dst.setAttempts(), 0) << "the transfer never even tried to set the time";
}

TEST(PreserveMtimeTest, RecursiveTransferDoesNotRelistPerFile) {
    // Restamping a copy needs the source's time. Looking it up per file means
    // listing the parent directory once per file -- N+1 round trips, which on a
    // single-channel backend like SMB is real latency. The recursion already
    // holds each child's FileInfo from the listing that enumerated it, so the
    // time must be handed down instead of rediscovered.
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const int fileCount = 20;
    const QString tree = QDir(srcDir.path()).filePath("tree");
    for (int i = 0; i < fileCount; ++i) {
        const QString f = writeFile(tree, QStringLiteral("f%1.txt").arg(i), "data");
        backdate(f, kOldEpochSecs + i);
    }

    CountingProvider provider;
    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.copyAcrossProviders(&provider, {tree}, &provider, dstDir.path(),
                                         /*removeSource=*/false, nullptr, &err))
        << err.toStdString();

    // Enumerating and sizing the tree are the only legitimate listings. Allowing
    // a small constant keeps the test from being brittle about how the byte
    // total is computed, while still failing loudly on anything per-file.
    EXPECT_LT(provider.listCalls(), fileCount)
        << "listed " << provider.listCalls() << " times for " << fileCount << " files";

    // ...and the timestamps still came across correctly.
    const QDir out(QDir(dstDir.path()).filePath("tree"));
    for (int i = 0; i < fileCount; ++i) {
        const QString name = QStringLiteral("f%1.txt").arg(i);
        EXPECT_EQ(mtimeOf(out.filePath(name)), mtimeOf(QDir(tree).filePath(name))) << i;
    }
}

TEST(PreserveMtimeTest, LocalProviderSetModifiedTimeRoundTrips) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeFile(dir.path(), "stamp.txt", "x");
    const QDateTime want = QDateTime::fromSecsSinceEpoch(kOldEpochSecs);

    EXPECT_TRUE(LocalFileProvider::instance()->setModifiedTime(path, want));
    EXPECT_EQ(mtimeOf(path), want);

    // An invalid time is rejected rather than clearing the file's stamp.
    EXPECT_FALSE(LocalFileProvider::instance()->setModifiedTime(path, QDateTime()));
    EXPECT_EQ(mtimeOf(path), want);
}

TEST(PreserveMtimeTest, OverwritingAnExistingFileAdoptsTheSourceTime) {
    // The path a real sync takes for a file present on both sides: the
    // destination already exists and is NEWER than what replaces it. If the
    // overwrite left the destination's own stamp in place, the very next
    // comparison would still call the two sides different -- the copy would
    // have happened for nothing, every single time.
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const QString source = writeFile(srcDir.path(), "both.txt", "authoritative");
    backdate(source);
    // Destination is dated now, i.e. newer than the source about to land on it.
    writeFile(dstDir.path(), "both.txt", "stale");

    FileOperations ops;
    ConflictResolver overwrite = [](const QString &, const QString &) {
        return ErrorAction::OverwriteAll;
    };
    QString err;
    ASSERT_TRUE(ops.copyPaths({source}, dstDir.path(), overwrite, &err)) << err.toStdString();

    const QString target = QDir(dstDir.path()).filePath("both.txt");
    QFile written(target);
    ASSERT_TRUE(written.open(QIODevice::ReadOnly));
    EXPECT_EQ(written.readAll(), QByteArray("authoritative"));
    EXPECT_EQ(mtimeOf(target), mtimeOf(source));
}
