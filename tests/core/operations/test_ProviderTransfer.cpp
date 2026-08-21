#include <gtest/gtest.h>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <cstring>

#include "FileOperations.h"
#include "FileProvider.h"
#include "LocalFileProvider.h"

// Cross-provider transfer engine tests. There is no SFTP server available in
// CI, so both sides use LocalFileProvider — the streaming/resume code path is
// provider-agnostic, so exercising it locally covers the same logic a
// local<->remote transfer runs.
namespace {

QString writeFile(const QString &dir, const QString &name, const QByteArray &content) {
    const QString path = QDir(dir).filePath(name);
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write(content);
    file.close();
    return path;
}

QByteArray readFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    return file.readAll();
}

// A payload with position-dependent bytes so any duplication, truncation, or
// misaligned resume corrupts the content in a way the assertions catch.
QByteArray patternedPayload(int size) {
    QByteArray data;
    data.resize(size);
    for (int i = 0; i < size; ++i)
        data[i] = static_cast<char>(i % 251);
    return data;
}

class InMemoryProvider : public FileProvider {
public:
    struct ReadHandle : FileHandle {
        QString path;
        qint64 offset = 0;
    };

    struct WriteHandle : FileHandle {
        QString path;
        qint64 offset = 0;
        StreamError error = StreamError::None;
        QString errorDetail;

        StreamError streamError() const override { return error; }
        QString streamErrorDetail() const override { return errorDetail; }
    };

    QVector<FileInfo> list(const QString &, bool) const override { return {}; }
    bool isDir(const QString &) const override { return false; }
    QString cleanPath(const QString &path) const override { return path; }
    QString parentPath(const QString &path) const override {
        const int slash = path.lastIndexOf(QLatin1Char('/'));
        return slash <= 0 ? QStringLiteral("/") : path.left(slash);
    }
    bool exists(const QString &path) const override { return m_files.contains(path); }
    RenameResult rename(const QString &, const QString &, QString *) override {
        return RenameResult::Failed;
    }
    FileHandle *openRead(const QString &path) override {
        if (!m_files.contains(path))
            return nullptr;
        auto *handle = new ReadHandle;
        handle->path = path;
        return handle;
    }
    FileHandle *openWrite(const QString &path, bool truncate) override {
        if (truncate)
            m_files.insert(path, {});
        else if (!m_files.contains(path))
            return nullptr;
        auto *handle = new WriteHandle;
        handle->path = path;
        return handle;
    }
    qint64 read(FileHandle *handle, char *buffer, qint64 maxSize) override {
        auto *input = static_cast<ReadHandle *>(handle);
        if (m_growthPending) {
            m_files[input->path].append(m_growth);
            m_growthPending = false;
        }
        if (m_readFailureOffsets.value(input->path, -1) == input->offset)
            return -1;
        const QByteArray &data = m_files[input->path];
        if (input->offset >= data.size())
            return 0;
        const qint64 count = qMin(maxSize, qint64(data.size()) - input->offset);
        std::memcpy(buffer, data.constData() + input->offset, static_cast<size_t>(count));
        input->offset += count;
        if (m_shrinkAfterFirstReadSizes.contains(input->path))
            m_files[input->path].truncate(m_shrinkAfterFirstReadSizes.take(input->path));
        return count;
    }
    qint64 write(FileHandle *handle, const char *buffer, qint64 size) override {
        auto *output = static_cast<WriteHandle *>(handle);
        if (m_writeFailure != FileHandle::StreamError::None) {
            output->error = m_writeFailure;
            output->errorDetail = m_writeFailureDetail;
            return -1;
        }
        if (size == 0) {
            ++m_zeroWriteCalls;
            return m_failZeroWrites ? -1 : 0;
        }
        QByteArray &data = m_files[output->path];
        if (output->offset + size > data.size())
            data.resize(output->offset + size);
        std::memcpy(data.data() + output->offset, buffer, static_cast<size_t>(size));
        output->offset += size;
        return size;
    }
    bool seek(FileHandle *handle, qint64 offset) override {
        if (auto *input = dynamic_cast<ReadHandle *>(handle)) {
            input->offset = offset;
            return true;
        }
        auto *output = dynamic_cast<WriteHandle *>(handle);
        if (!output)
            return false;
        output->offset = offset;
        return true;
    }
    bool supportsWriteResume() const override { return m_supportsWriteResume; }
    qint64 handleSize(FileHandle *handle) override {
        if (auto *input = dynamic_cast<ReadHandle *>(handle)) {
            const QVector<qint64> sizes = m_reportedSizeSequences.value(input->path);
            const int call = m_handleSizeCalls[input->path]++;
            if (call < sizes.size())
                return sizes.at(call);
            return m_reportedSizes.value(input->path, m_files.value(input->path).size());
        }
        auto *output = dynamic_cast<WriteHandle *>(handle);
        return output ? m_files.value(output->path).size() : -1;
    }
    void setExpectedWriteSize(FileHandle *, qint64 size) override {
        m_expectedWriteSizes.append(size);
    }
    void closeHandle(FileHandle *handle) override {
        if (dynamic_cast<ReadHandle *>(handle))
            ++m_closeReadCalls;
        delete handle;
    }
    bool closeHandleStatus(FileHandle *handle) override {
        return closeHandleResult(handle).committed;
    }
    CloseHandleResult closeHandleResult(FileHandle *handle) override {
        ++m_closeWriteCalls;
        delete handle;
        return {!m_failCloseWrites, m_closeFailure, m_closeFailureDetail};
    }
    bool canStream() const override { return true; }
    bool remove(const QString &path) override { return m_files.remove(path) > 0; }
    bool mkdir(const QString &) override { return true; }

    void addFile(const QString &path, const QByteArray &data) { m_files.insert(path, data); }
    void reportSize(const QString &path, qint64 size) { m_reportedSizes.insert(path, size); }
    void reportSizeSequence(const QString &path, const QVector<qint64> &sizes) {
        m_reportedSizeSequences.insert(path, sizes);
    }
    void failReadAtOffset(const QString &path, qint64 offset) {
        m_readFailureOffsets.insert(path, offset);
    }
    void shrinkAfterFirstRead(const QString &path, qint64 size) {
        m_shrinkAfterFirstReadSizes.insert(path, size);
    }
    void failZeroWrites() { m_failZeroWrites = true; }
    void failWrites(FileHandle::StreamError error, const QString &detail = {}) {
        m_writeFailure = error;
        m_writeFailureDetail = detail;
    }
    void disableWriteResume() { m_supportsWriteResume = false; }
    void failCloseWrites(FileHandle::StreamError error = FileHandle::StreamError::Other,
                         const QString &detail = {}) {
        m_failCloseWrites = true;
        m_closeFailure = error;
        m_closeFailureDetail = detail;
    }
    void growOnFirstRead(const QByteArray &data) {
        m_growth = data;
        m_growthPending = true;
    }
    QByteArray file(const QString &path) const { return m_files.value(path); }
    const QVector<qint64> &expectedWriteSizes() const { return m_expectedWriteSizes; }
    int closeWriteCalls() const { return m_closeWriteCalls; }
    int closeReadCalls() const { return m_closeReadCalls; }
    int zeroWriteCalls() const { return m_zeroWriteCalls; }

private:
    QHash<QString, QByteArray> m_files;
    QHash<QString, qint64> m_reportedSizes;
    QHash<QString, QVector<qint64>> m_reportedSizeSequences;
    QHash<QString, int> m_handleSizeCalls;
    QHash<QString, qint64> m_readFailureOffsets;
    QHash<QString, qint64> m_shrinkAfterFirstReadSizes;
    QByteArray m_growth;
    bool m_growthPending = false;
    bool m_failZeroWrites = false;
    FileHandle::StreamError m_writeFailure = FileHandle::StreamError::None;
    QString m_writeFailureDetail;
    bool m_supportsWriteResume = true;
    bool m_failCloseWrites = false;
    FileHandle::StreamError m_closeFailure = FileHandle::StreamError::None;
    QString m_closeFailureDetail;
    QVector<qint64> m_expectedWriteSizes;
    int m_closeWriteCalls = 0;
    int m_closeReadCalls = 0;
    int m_zeroWriteCalls = 0;
};

} // namespace

TEST(ProviderTransferTest, CopiesSingleFileByteForByte) {
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const QByteArray payload = patternedPayload(200000); // spans many 64 KiB chunks
    const QString source = writeFile(srcDir.path(), "file.bin", payload);

    auto *provider = LocalFileProvider::instance();
    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.copyAcrossProviders(provider, {source}, provider, dstDir.path(),
                                        /*removeSource=*/false, nullptr, &err))
        << err.toStdString();

    EXPECT_TRUE(QFile::exists(source)); // copy leaves the source in place
    EXPECT_EQ(readFile(QDir(dstDir.path()).filePath("file.bin")), payload);
}

TEST(ProviderTransferTest, CopiesDirectoryRecursively) {
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    QDir(srcDir.path()).mkdir("tree");
    const QString treeRoot = QDir(srcDir.path()).filePath("tree");
    QDir(treeRoot).mkdir("nested");
    const QByteArray top = QByteArray("top-level file");
    const QByteArray inner = QByteArray("deeply nested file");
    writeFile(treeRoot, "top.txt", top);
    writeFile(QDir(treeRoot).filePath("nested"), "inner.txt", inner);

    auto *provider = LocalFileProvider::instance();
    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.copyAcrossProviders(provider, {treeRoot}, provider, dstDir.path(),
                                        /*removeSource=*/false, nullptr, &err))
        << err.toStdString();

    EXPECT_EQ(readFile(QDir(dstDir.path()).filePath("tree/top.txt")), top);
    EXPECT_EQ(readFile(QDir(dstDir.path()).filePath("tree/nested/inner.txt")), inner);
}

TEST(ProviderTransferTest, ResumesPartialDestination) {
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const QByteArray payload = patternedPayload(300000);
    const QString source = writeFile(srcDir.path(), "resume.bin", payload);

    // Simulate an interrupted transfer: the destination already holds the first
    // N bytes of the source (a valid prefix).
    const int partial = 123456;
    writeFile(dstDir.path(), "resume.bin", payload.left(partial));

    auto *provider = LocalFileProvider::instance();
    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.copyAcrossProviders(provider, {source}, provider, dstDir.path(),
                                        /*removeSource=*/false, nullptr, &err))
        << err.toStdString();

    // The final file must equal the whole source — neither duplicated (which
    // would grow it past payload.size()) nor corrupted at the resume seam.
    const QByteArray result = readFile(QDir(dstDir.path()).filePath("resume.bin"));
    EXPECT_EQ(result.size(), payload.size());
    EXPECT_EQ(result, payload);
}

TEST(ProviderTransferTest, NonResumableDestinationOverwritesPartialFile) {
    InMemoryProvider src;
    InMemoryProvider dst;
    const QByteArray payload = patternedPayload(64 * 1024);
    src.addFile("/source/payload.bin", payload);
    dst.addFile("/destination/payload.bin", payload.left(16 * 1024));
    dst.disableWriteResume();

    FileOperations ops;
    const ConflictResolver overwrite = [](const FileConflict &) { return ErrorAction::Overwrite; };
    QString error;
    ASSERT_TRUE(ops.copyAcrossProviders(&src, {"/source/payload.bin"}, &dst, "/destination",
                                        /*removeSource=*/false, overwrite, &error));

    EXPECT_EQ(dst.file("/destination/payload.bin"), payload);
    ASSERT_EQ(dst.expectedWriteSizes().size(), 1);
    EXPECT_EQ(dst.expectedWriteSizes().front(), payload.size());
}

TEST(ProviderTransferTest, MoveRemovesSource) {
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const QByteArray payload = QByteArray("payload to relocate");
    const QString source = writeFile(srcDir.path(), "move.txt", payload);

    auto *provider = LocalFileProvider::instance();
    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.copyAcrossProviders(provider, {source}, provider, dstDir.path(),
                                        /*removeSource=*/true, nullptr, &err))
        << err.toStdString();

    EXPECT_FALSE(QFile::exists(source)); // move removes the source
    EXPECT_EQ(readFile(QDir(dstDir.path()).filePath("move.txt")), payload);
}

TEST(ProviderTransferTest, KnownSizeCopyRejectsSourceGrowthAfterInitialSnapshot) {
    const QByteArray initial = QByteArray("initial snapshot");
    InMemoryProvider src;
    InMemoryProvider dst;
    src.addFile("/source/growing.bin", initial);
    src.growOnFirstRead(QByteArray(" appended after the copy started"));

    FileOperations ops;
    ops.setErrorResolver([](const QString &, const QString &) { return ErrorAction::Cancel; });
    QString error;
    ASSERT_FALSE(ops.copyAcrossProviders(&src, {"/source/growing.bin"}, &dst, "/destination",
                                          /*removeSource=*/false, nullptr, &error));

    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(dst.file("/destination/growing.bin"), initial);
    ASSERT_EQ(dst.expectedWriteSizes().size(), 1);
    EXPECT_EQ(dst.expectedWriteSizes().front(), initial.size());
    EXPECT_EQ(dst.closeWriteCalls(), 1);
}

TEST(ProviderTransferTest, KnownSizeProbeReadErrorFailsAfterDestinationClose) {
    InMemoryProvider src;
    InMemoryProvider dst;
    src.addFile("/source/probe-error.bin", QByteArray("abc"));
    src.failReadAtOffset("/source/probe-error.bin", 3);

    FileOperations ops;
    QString error;
    ASSERT_FALSE(ops.copyAcrossProviders(&src, {"/source/probe-error.bin"}, &dst, "/destination",
                                          /*removeSource=*/false, nullptr, &error));

    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(dst.file("/destination/probe-error.bin"), QByteArray("abc"));
    EXPECT_EQ(dst.closeWriteCalls(), 1);
}

TEST(ProviderTransferTest, ResumedKnownSizeCopyRejectsSourceGrowth) {
    const QByteArray initial = QByteArray("0123456789");
    const qint64 startOffset = 4;
    InMemoryProvider src;
    InMemoryProvider dst;
    src.addFile("/source/resume.bin", initial);
    src.growOnFirstRead(QByteArray("later growth"));
    dst.addFile("/destination/resume.bin", initial.left(startOffset));

    FileOperations ops;
    ops.setErrorResolver([](const QString &, const QString &) { return ErrorAction::Cancel; });
    QString error;
    ASSERT_FALSE(ops.copyAcrossProviders(&src, {"/source/resume.bin"}, &dst, "/destination",
                                          /*removeSource=*/false, nullptr, &error));

    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(dst.file("/destination/resume.bin"), initial);
    ASSERT_EQ(dst.expectedWriteSizes().size(), 1);
    EXPECT_EQ(dst.expectedWriteSizes().front(), initial.size() - startOffset);
    EXPECT_EQ(dst.closeWriteCalls(), 1);
}

TEST(ProviderTransferTest, RejectsResumeOffsetPastKnownCurrentSourceSize) {
    InMemoryProvider src;
    InMemoryProvider dst;
    src.addFile("/source/shrunk.bin", QByteArray("1234"));
    // Sizing before resume observes the old 10-byte file; the stream handle
    // observes the already-shrunk 4-byte file after the destination chose offset 6.
    src.reportSizeSequence("/source/shrunk.bin", {10, 10, 4});
    dst.addFile("/destination/shrunk.bin", QByteArray("123456"));

    FileOperations ops;
    ops.setErrorResolver([](const QString &, const QString &) { return ErrorAction::Cancel; });
    QString error;
    ASSERT_FALSE(ops.copyAcrossProviders(&src, {"/source/shrunk.bin"}, &dst, "/destination",
                                          /*removeSource=*/true, nullptr, &error));

    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(src.file("/source/shrunk.bin"), QByteArray("1234"));
    EXPECT_EQ(dst.file("/destination/shrunk.bin"), QByteArray("123456"));
    EXPECT_EQ(src.closeReadCalls(), 3);
    EXPECT_EQ(dst.closeWriteCalls(), 1);
}

TEST(ProviderTransferTest, RejectsObservableSourceShrinkAfterExpectedBytes) {
    const QByteArray initial = QByteArray("abcdef");
    InMemoryProvider src;
    InMemoryProvider dst;
    src.addFile("/source/truncate-after-read.bin", initial);
    src.shrinkAfterFirstRead("/source/truncate-after-read.bin", 3);

    FileOperations ops;
    ops.setErrorResolver([](const QString &, const QString &) { return ErrorAction::Cancel; });
    QString error;
    ASSERT_FALSE(ops.copyAcrossProviders(&src, {"/source/truncate-after-read.bin"}, &dst,
                                          "/destination", /*removeSource=*/true, nullptr, &error));

    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(src.file("/source/truncate-after-read.bin"), QByteArray("abc"));
    EXPECT_EQ(dst.file("/destination/truncate-after-read.bin"), initial);
    EXPECT_EQ(dst.closeWriteCalls(), 1);
}

TEST(ProviderTransferTest, ObservableSourceShrinkRollsBackProgressBeforeNextFile) {
    const QByteArray initial = QByteArray("abcdef");
    InMemoryProvider src;
    InMemoryProvider dst;
    src.addFile("/source/truncate-after-read.bin", initial);
    src.shrinkAfterFirstRead("/source/truncate-after-read.bin", 3);
    src.addFile("/source/valid.bin", QByteArray("done"));

    QVector<qint64> observedProgress;
    FileOperations ops;
    QObject::connect(&ops, &FileOperations::progress,
                     [&observedProgress](qint64, qint64, qint64 doneBytes, qint64,
                                         const QString &) { observedProgress.append(doneBytes); });

    QString error;
    ASSERT_FALSE(ops.copyAcrossProviders(&src,
                                          {"/source/truncate-after-read.bin", "/source/valid.bin"},
                                          &dst, "/destination", /*removeSource=*/false, nullptr,
                                          &error));

    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(dst.file("/destination/truncate-after-read.bin"), initial);
    EXPECT_EQ(dst.file("/destination/valid.bin"), QByteArray("done"));
    ASSERT_FALSE(observedProgress.isEmpty());
    EXPECT_EQ(observedProgress.back(), 4);
}

TEST(ProviderTransferTest, EmptyKnownSizeCopyStartsZeroByteWrite) {
    InMemoryProvider src;
    InMemoryProvider dst;
    src.addFile("/source/empty.bin", {});

    FileOperations ops;
    QString error;
    ASSERT_TRUE(ops.copyAcrossProviders(&src, {"/source/empty.bin"}, &dst, "/destination",
                                         /*removeSource=*/false, nullptr, &error))
        << error.toStdString();

    EXPECT_EQ(dst.file("/destination/empty.bin"), QByteArray());
    EXPECT_EQ(dst.expectedWriteSizes(), QVector<qint64>{0});
    EXPECT_EQ(dst.zeroWriteCalls(), 1);
    EXPECT_EQ(dst.closeWriteCalls(), 1);
}

TEST(ProviderTransferTest, EmptyKnownSizeCopyFailsWhenZeroByteWriteFails) {
    InMemoryProvider src;
    InMemoryProvider dst;
    src.addFile("/source/empty.bin", {});
    dst.failZeroWrites();

    FileOperations ops;
    ops.setErrorResolver([](const QString &, const QString &) { return ErrorAction::Cancel; });
    QString error;
    ASSERT_FALSE(ops.copyAcrossProviders(&src, {"/source/empty.bin"}, &dst, "/destination",
                                          /*removeSource=*/false, nullptr, &error));

    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(dst.zeroWriteCalls(), 1);
    EXPECT_EQ(dst.closeWriteCalls(), 1);
}

TEST(ProviderTransferTest, FailedDestinationCloseRetainsMoveSource) {
    InMemoryProvider src;
    InMemoryProvider dst;
    const QByteArray payload("close result must commit the upload");
    src.addFile("/source/commit.bin", payload);
    dst.failCloseWrites();

    FileOperations ops;
    ops.setErrorResolver([](const QString &, const QString &) { return ErrorAction::Cancel; });
    QString error;
    ASSERT_FALSE(ops.copyAcrossProviders(&src, {"/source/commit.bin"}, &dst, "/destination",
                                          /*removeSource=*/true, nullptr, &error));

    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(src.file("/source/commit.bin"), payload);
    EXPECT_EQ(dst.file("/destination/commit.bin"), payload);
    EXPECT_EQ(dst.closeWriteCalls(), 1);
}

TEST(ProviderTransferTest, WriteFailureReportsDestinationOutOfSpace) {
    InMemoryProvider src;
    InMemoryProvider dst;
    src.addFile("/source/payload.bin", QByteArray("payload"));
    dst.failWrites(FileHandle::StreamError::NoSpace);

    FileOperations ops;
    QSignalSpy errors(&ops, &FileOperations::errorOccurred);
    ops.setErrorResolver([](const QString &, const QString &) { return ErrorAction::Cancel; });
    QString error;
    ASSERT_FALSE(ops.copyAcrossProviders(&src, {"/source/payload.bin"}, &dst, "/destination",
                                          /*removeSource=*/false, nullptr, &error));

    const QString expected = QStringLiteral("The destination has no space for /destination/payload.bin");
    EXPECT_EQ(error, expected);
    ASSERT_EQ(errors.count(), 1);
    EXPECT_EQ(errors.at(0).at(0).toString(), expected);
}

TEST(ProviderTransferTest, WriteFailureReportsPermissionDenied) {
    InMemoryProvider src;
    InMemoryProvider dst;
    src.addFile("/source/payload.bin", QByteArray("payload"));
    dst.failWrites(FileHandle::StreamError::PermissionDenied);

    FileOperations ops;
    ops.setErrorResolver([](const QString &, const QString &) { return ErrorAction::Cancel; });
    QString error;
    ASSERT_FALSE(ops.copyAcrossProviders(&src, {"/source/payload.bin"}, &dst, "/destination",
                                          /*removeSource=*/false, nullptr, &error));

    EXPECT_EQ(error, QStringLiteral("You do not have permission to write /destination/payload.bin"));
}

TEST(ProviderTransferTest, RemotePermissionFailureNeverInvokesPrivilegeBroker) {
    InMemoryProvider src;
    InMemoryProvider dst;
    src.addFile("/source/payload.bin", QByteArray("payload"));
    dst.failWrites(FileHandle::StreamError::PermissionDenied);

    FileOperations ops;
    int brokerCalls = 0;
    ops.setPrivilegeExecutor([&](const PrivilegedOperationRequest &) {
        ++brokerCalls;
        return PrivilegeResult{PrivilegeStatus::Succeeded, 0, {}};
    });
    ops.setErrorResolver([](const OperationError &error) {
        EXPECT_TRUE(error.remote);
        EXPECT_FALSE(error.elevatable);
        return ErrorAction::Elevate;
    });

    QString error;
    EXPECT_FALSE(ops.copyAcrossProviders(&src, {"/source/payload.bin"}, &dst, "/destination",
                                         false, nullptr, &error));
    EXPECT_EQ(brokerCalls, 0);
}

TEST(ProviderTransferTest, WriteFailureReportsConnectionLost) {
    InMemoryProvider src;
    InMemoryProvider dst;
    src.addFile("/source/payload.bin", QByteArray("payload"));
    dst.failWrites(FileHandle::StreamError::ConnectionLost);

    FileOperations ops;
    ops.setErrorResolver([](const QString &, const QString &) { return ErrorAction::Cancel; });
    QString error;
    ASSERT_FALSE(ops.copyAcrossProviders(&src, {"/source/payload.bin"}, &dst, "/destination",
                                          /*removeSource=*/false, nullptr, &error));

    EXPECT_EQ(error,
              QStringLiteral("Connection to the server was lost while transferring /destination/payload.bin"));
}

TEST(ProviderTransferTest, CloseFailureReportsCommitDetailAndRetainsMoveSource) {
    InMemoryProvider src;
    InMemoryProvider dst;
    const QByteArray payload("payload");
    src.addFile("/source/payload.bin", payload);
    dst.failCloseWrites(FileHandle::StreamError::Other, QStringLiteral("Input/output error"));

    FileOperations ops;
    QSignalSpy errors(&ops, &FileOperations::errorOccurred);
    ops.setErrorResolver([](const QString &, const QString &) { return ErrorAction::Cancel; });
    QString error;
    ASSERT_FALSE(ops.copyAcrossProviders(&src, {"/source/payload.bin"}, &dst, "/destination",
                                          /*removeSource=*/true, nullptr, &error));

    const QString expected =
        QStringLiteral("Upload of /destination/payload.bin did not complete: Input/output error");
    EXPECT_EQ(error, expected);
    ASSERT_EQ(errors.count(), 1);
    EXPECT_EQ(errors.at(0).at(0).toString(), expected);
    EXPECT_EQ(src.file("/source/payload.bin"), payload);
}

TEST(ProviderTransferTest, UnknownSizeCopyKeepsStreamingUntilEof) {
    const QByteArray initial = QByteArray("unknown");
    const QByteArray growth = QByteArray(" size growth");
    InMemoryProvider src;
    InMemoryProvider dst;
    src.addFile("/source/unknown.bin", initial);
    src.reportSize("/source/unknown.bin", -1);
    src.growOnFirstRead(growth);

    FileOperations ops;
    QString error;
    ASSERT_TRUE(ops.copyAcrossProviders(&src, {"/source/unknown.bin"}, &dst, "/destination",
                                         /*removeSource=*/false, nullptr, &error))
        << error.toStdString();

    EXPECT_EQ(dst.file("/destination/unknown.bin"), initial + growth);
    ASSERT_EQ(dst.expectedWriteSizes().size(), 1);
    EXPECT_EQ(dst.expectedWriteSizes().front(), -1);
}

TEST(ProviderTransferTest, KnownSizeEarlyEofFailsAndRollsBackTransferProgress) {
    InMemoryProvider src;
    InMemoryProvider dst;
    src.addFile("/source/short.bin", QByteArray("abc"));
    src.reportSize("/source/short.bin", 6);
    src.addFile("/source/valid.bin", QByteArray("done"));

    QVector<qint64> observedProgress;
    FileOperations ops;
    QObject::connect(&ops, &FileOperations::progress,
                     [&observedProgress](qint64, qint64, qint64 doneBytes, qint64,
                                         const QString &) { observedProgress.append(doneBytes); });

    QString error;
    ASSERT_FALSE(ops.copyAcrossProviders(&src, {"/source/short.bin", "/source/valid.bin"}, &dst,
                                          "/destination", /*removeSource=*/false, nullptr, &error));

    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(dst.file("/destination/short.bin"), QByteArray("abc"));
    EXPECT_EQ(dst.file("/destination/valid.bin"), QByteArray("done"));
    ASSERT_EQ(dst.expectedWriteSizes().size(), 2);
    EXPECT_EQ(dst.expectedWriteSizes().at(0), 6);
    EXPECT_EQ(dst.expectedWriteSizes().at(1), 4);
    EXPECT_EQ(dst.closeWriteCalls(), 2);
    ASSERT_FALSE(observedProgress.isEmpty());
    EXPECT_EQ(observedProgress.back(), 4);
}

TEST(ProviderTransferTest, ThrottledCopyPacesToTheRateLimit) {
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const int size = 512 * 1024; // 512 KiB
    const QByteArray payload = patternedPayload(size);
    const QString source = writeFile(srcDir.path(), "big.bin", payload);
    const QString target = QDir(dstDir.path()).filePath("big.bin");
    auto *provider = LocalFileProvider::instance();

    // Baseline without a cap: near-instant.
    QElapsedTimer clock;
    clock.start();
    {
        FileOperations ops;
        QString err;
        ASSERT_TRUE(ops.copyAcrossProviders(provider, {source}, provider, dstDir.path(),
                                            /*removeSource=*/false, nullptr, &err))
            << err.toStdString();
    }
    const qint64 unthrottledMs = clock.elapsed();
    QFile::remove(target);

    // Capped to 512 KiB/s: the same 512 KiB has to take ~1 s.
    clock.restart();
    {
        FileOperations ops;
        ops.setTransferRateLimit(512 * 1024);
        QString err;
        ASSERT_TRUE(ops.copyAcrossProviders(provider, {source}, provider, dstDir.path(),
                                            /*removeSource=*/false, nullptr, &err))
            << err.toStdString();
    }
    const qint64 throttledMs = clock.elapsed();

    EXPECT_GE(throttledMs, 500) << "throttled copy finished in " << throttledMs
                                << " ms, far under the 512 KiB/s pace";
    EXPECT_GT(throttledMs, unthrottledMs * 2)
        << "throttled (" << throttledMs << " ms) was not slower than unthrottled ("
        << unthrottledMs << " ms)";
    EXPECT_EQ(readFile(target), payload);
}
