#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QHash>
#include <QTemporaryDir>
#include <cstring>

#include "FileOperations.h"
#include "FileProvider.h"

// What the overwrite prompt is told about the two files.
//
// It used to be told nothing: the prompt ran a QFileInfo over each path, and on
// a transfer between a server and this machine the source path is the server's,
// so QFileInfo described a same-named local file or nothing at all. Both sides
// read "(0 bytes)" and the user chose whether to overwrite from that.
namespace {

// A minimal streaming backend holding its files in memory, standing in for a
// server: its paths mean nothing on this machine, which is the whole point.
class MemoryProvider : public FileProvider {
public:
    struct Handle : FileHandle {
        QString path;
        qint64 offset = 0;
    };

    void put(const QString &path, const QByteArray &data) { m_files.insert(path, data); }

    QVector<FileInfo> list(const QString &, bool) const override { return {}; }
    bool isDir(const QString &) const override { return false; }
    QString cleanPath(const QString &p) const override { return p; }
    QString parentPath(const QString &path) const override {
        const int slash = path.lastIndexOf(QLatin1Char('/'));
        return slash <= 0 ? QStringLiteral("/") : path.left(slash);
    }
    bool exists(const QString &path) const override { return m_files.contains(path); }
    RenameResult rename(const QString &, const QString &, QString *) override {
        return RenameResult::Failed;
    }
    bool mkdir(const QString &) override { return true; }
    bool canStream() const override { return true; }

    FileHandle *openRead(const QString &path) override {
        if (!m_files.contains(path))
            return nullptr;
        auto *h = new Handle;
        h->path = path;
        return h;
    }
    FileHandle *openWrite(const QString &path, bool truncate) override {
        if (truncate)
            m_files.insert(path, {});
        else if (!m_files.contains(path))
            return nullptr;
        auto *h = new Handle;
        h->path = path;
        return h;
    }
    qint64 read(FileHandle *handle, char *buffer, qint64 maxSize) override {
        auto *h = static_cast<Handle *>(handle);
        const QByteArray &data = m_files[h->path];
        if (h->offset >= data.size())
            return 0;
        const qint64 n = qMin(maxSize, qint64(data.size()) - h->offset);
        std::memcpy(buffer, data.constData() + h->offset, static_cast<size_t>(n));
        h->offset += n;
        return n;
    }
    qint64 write(FileHandle *handle, const char *buffer, qint64 size) override {
        auto *h = static_cast<Handle *>(handle);
        QByteArray &data = m_files[h->path];
        if (h->offset + size > data.size())
            data.resize(h->offset + size);
        std::memcpy(data.data() + h->offset, buffer, static_cast<size_t>(size));
        h->offset += size;
        return size;
    }
    bool seek(FileHandle *handle, qint64 offset) override {
        static_cast<Handle *>(handle)->offset = offset;
        return true;
    }
    qint64 handleSize(FileHandle *handle) override {
        return m_files.value(static_cast<Handle *>(handle)->path).size();
    }

private:
    QHash<QString, QByteArray> m_files;
};

QString writeFile(const QString &dir, const QString &name, int bytes) {
    const QString path = QDir(dir).filePath(name);
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write(QByteArray(bytes, 'x'));
    file.close();
    return path;
}

} // namespace

TEST(ConflictSizesTest, CrossProviderTransferReportsTheRealSizesOfBothFiles) {
    MemoryProvider src, dst;
    // The destination is LARGER than the source, which is what makes this a
    // conflict rather than a resume (a shorter destination is resumed from).
    src.put(QStringLiteral("/share/report.pdf"), QByteArray(1234567, 'a'));
    dst.put(QStringLiteral("/backup/report.pdf"), QByteArray(2000000, 'b'));

    FileConflict seen;
    int prompts = 0;
    ConflictResolver resolver = [&](const FileConflict &conflict) {
        seen = conflict;
        ++prompts;
        return ErrorAction::Skip;
    };

    FileOperations ops;
    QString err;
    ops.copyAcrossProviders(&src, {QStringLiteral("/share/report.pdf")}, &dst,
                            QStringLiteral("/backup"), /*removeSource=*/false, resolver, &err);

    ASSERT_EQ(prompts, 1) << "no overwrite prompt was raised";
    EXPECT_EQ(seen.sourcePath.toStdString(), "/share/report.pdf");
    EXPECT_EQ(seen.destPath.toStdString(), "/backup/report.pdf");
    EXPECT_EQ(seen.sourceSize, 1234567)
        << "the prompt would have shown " << seen.sourceSize << " for a 1234567-byte file";
    EXPECT_EQ(seen.destSize, 2000000);
}

TEST(ConflictSizesTest, LocalCopyStillReportsTheRealSizes) {
    // The local path must keep working: these sizes were right before, and the
    // prompt still has to show them.
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const QString source = writeFile(srcDir.path(), QStringLiteral("note.txt"), 11);
    writeFile(dstDir.path(), QStringLiteral("note.txt"), 5);

    FileConflict seen;
    int prompts = 0;
    ConflictResolver resolver = [&](const FileConflict &conflict) {
        seen = conflict;
        ++prompts;
        return ErrorAction::Skip;
    };

    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.copyPaths({source}, dstDir.path(), resolver, &err));

    ASSERT_EQ(prompts, 1);
    EXPECT_EQ(seen.sourcePath, source);
    EXPECT_EQ(seen.sourceSize, 11);
    EXPECT_EQ(seen.destSize, 5);
}

TEST(ConflictSizesTest, DirectoryConflictReportsUnknownRatherThanZero) {
    // A directory has no single size. Reporting 0 would read as "an empty file"
    // in the prompt; -1 lets it say the size is unknown.
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    ASSERT_TRUE(QDir(srcDir.path()).mkpath(QStringLiteral("folder")));
    ASSERT_TRUE(QDir(dstDir.path()).mkpath(QStringLiteral("folder")));
    writeFile(QDir(srcDir.path()).filePath(QStringLiteral("folder")), QStringLiteral("a.txt"), 3);

    FileConflict seen;
    int prompts = 0;
    ConflictResolver resolver = [&](const FileConflict &conflict) {
        seen = conflict;
        ++prompts;
        return ErrorAction::Skip;
    };

    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.copyPaths({QDir(srcDir.path()).filePath(QStringLiteral("folder"))},
                              dstDir.path(), resolver, &err));

    ASSERT_EQ(prompts, 1);
    EXPECT_EQ(seen.sourceSize, -1);
    EXPECT_EQ(seen.destSize, -1);
}
