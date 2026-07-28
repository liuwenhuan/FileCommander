#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QSignalSpy>

#include "LocalFileProvider.h"
#include "OperationQueue.h"

// Exercises the threaded orchestration layer (worker QThread + queued dispatch
// + cross-thread conflict prompt), which the FileOperations unit tests don't
// cover. Relies on the QApplication event loop from test_main.cpp.

namespace {

QString writeFile(const QString &dir, const QString &name, const QByteArray &data = "data") {
    const QString path = QDir(dir).filePath(name);
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(data);
    f.close();
    return path;
}

bool waitFinished(QSignalSpy &spy) {
    return spy.count() > 0 || spy.wait(3000);
}

} // namespace

TEST(OperationQueueTest, EnqueueCopyCompletesAndCopiesFile) {
    QTemporaryDir src, dst;
    ASSERT_TRUE(src.isValid() && dst.isValid());
    const QString source = writeFile(src.path(), "x.txt", "payload");

    OperationQueue queue;
    QSignalSpy finished(&queue, &OperationQueue::finished);
    queue.enqueueCopy({source}, dst.path());

    ASSERT_TRUE(waitFinished(finished));
    EXPECT_TRUE(finished.takeFirst().at(0).toBool());
    EXPECT_TRUE(QFile::exists(source));
    EXPECT_TRUE(QFile::exists(QDir(dst.path()).filePath("x.txt")));
}

TEST(OperationQueueTest, EnqueueMoveRemovesSource) {
    QTemporaryDir src, dst;
    ASSERT_TRUE(src.isValid() && dst.isValid());
    const QString source = writeFile(src.path(), "y.txt");

    OperationQueue queue;
    QSignalSpy finished(&queue, &OperationQueue::finished);
    queue.enqueueMove({source}, dst.path());

    ASSERT_TRUE(waitFinished(finished));
    EXPECT_FALSE(QFile::exists(source));
    EXPECT_TRUE(QFile::exists(QDir(dst.path()).filePath("y.txt")));
}

TEST(OperationQueueTest, ConflictHandlerRunsAndOverwriteAllApplies) {
    QTemporaryDir src, dst;
    ASSERT_TRUE(src.isValid() && dst.isValid());
    const QString source = writeFile(src.path(), "c.txt", "fresh");
    writeFile(dst.path(), "c.txt", "stale");

    OperationQueue queue;
    int prompts = 0;
    queue.setConflictHandler([&prompts](const FileConflict &) {
        ++prompts;
        return ErrorAction::OverwriteAll;
    });

    QSignalSpy finished(&queue, &OperationQueue::finished);
    queue.enqueueCopy({source}, dst.path());
    ASSERT_TRUE(waitFinished(finished));

    EXPECT_EQ(prompts, 1);
    QFile out(QDir(dst.path()).filePath("c.txt"));
    ASSERT_TRUE(out.open(QIODevice::ReadOnly));
    EXPECT_EQ(out.readAll(), QByteArray("fresh"));
}

TEST(OperationQueueTest, EnqueueSymlinkCreatesLink) {
#ifdef Q_OS_WIN
    GTEST_SKIP() << "Windows symbolic links require Developer Mode or elevation";
#endif
    QTemporaryDir src, dst;
    ASSERT_TRUE(src.isValid() && dst.isValid());
    const QString source = writeFile(src.path(), "t.txt", "linked");

    OperationQueue queue;
    QSignalSpy finished(&queue, &OperationQueue::finished);
    queue.enqueueSymlink({source}, dst.path());

    ASSERT_TRUE(waitFinished(finished));
    EXPECT_TRUE(QFileInfo(QDir(dst.path()).filePath("t.txt")).isSymLink());
}

// --- Provider-transfer worker pool -----------------------------------------
//
// enqueueProviderCopy/Move are dispatched to a separate pool of transfer
// workers (see OperationQueue::setMaxConcurrentTransfers), entirely apart
// from the local-job pipeline exercised above. LocalFileProvider stands in
// for a remote backend here since it also implements the streaming
// FileProvider interface (canStream() == true), so these tests can run
// without a live SFTP/FTP/WebDAV server.

TEST(OperationQueueTest, ProviderCopySingleWorkerCompletesAndCopiesFile) {
    QTemporaryDir src, dst;
    ASSERT_TRUE(src.isValid() && dst.isValid());
    const QString source = writeFile(src.path(), "p.txt", "remote-payload");

    LocalFileProvider srcProvider;
    LocalFileProvider dstProvider;

    OperationQueue queue;
    queue.setMaxConcurrentTransfers(1); // pinned to the historical one-at-a-time behaviour
    QSignalSpy finished(&queue, &OperationQueue::finished);
    queue.enqueueProviderCopy(&srcProvider, {source}, &dstProvider, dst.path());

    ASSERT_TRUE(waitFinished(finished));
    EXPECT_TRUE(finished.takeFirst().at(0).toBool());
    EXPECT_TRUE(QFile::exists(source));
    EXPECT_TRUE(QFile::exists(QDir(dst.path()).filePath("p.txt")));
}

TEST(OperationQueueTest, ProviderMoveRemovesSource) {
    QTemporaryDir src, dst;
    ASSERT_TRUE(src.isValid() && dst.isValid());
    const QString source = writeFile(src.path(), "q.txt", "remote-move");

    LocalFileProvider srcProvider;
    LocalFileProvider dstProvider;

    OperationQueue queue;
    QSignalSpy finished(&queue, &OperationQueue::finished);
    queue.enqueueProviderMove(&srcProvider, {source}, &dstProvider, dst.path());

    ASSERT_TRUE(waitFinished(finished));
    EXPECT_FALSE(QFile::exists(source));
    EXPECT_TRUE(QFile::exists(QDir(dst.path()).filePath("q.txt")));
}

TEST(OperationQueueTest, ProviderTransfersRunConcurrentlyWithPoolOfTwo) {
    // Two independent provider-copy jobs queued back-to-back with a pool of 2
    // should both complete (proving the second worker actually picks up a job
    // rather than the pool silently behaving as size 1), and local jobs must
    // remain completely unaffected by any of this.
    QTemporaryDir src, dst;
    ASSERT_TRUE(src.isValid() && dst.isValid());
    const QString sourceA = writeFile(src.path(), "a.txt", "AAAA");
    const QString sourceB = writeFile(src.path(), "b.txt", "BBBB");

    LocalFileProvider srcProvider;
    LocalFileProvider dstProvider;

    OperationQueue queue;
    queue.setMaxConcurrentTransfers(2);
    EXPECT_EQ(queue.maxConcurrentTransfers(), 2);

    QSignalSpy finished(&queue, &OperationQueue::finished);
    queue.enqueueProviderCopy(&srcProvider, {sourceA}, &dstProvider, dst.path());
    queue.enqueueProviderCopy(&srcProvider, {sourceB}, &dstProvider, dst.path());

    // Wait for both jobs to report finished.
    while (finished.count() < 2 && finished.wait(3000)) {
    }
    ASSERT_EQ(finished.count(), 2);
    EXPECT_TRUE(QFile::exists(QDir(dst.path()).filePath("a.txt")));
    EXPECT_TRUE(QFile::exists(QDir(dst.path()).filePath("b.txt")));
}

TEST(OperationQueueTest, ProviderAndLocalJobsBothCompleteIndependently) {
    // Local jobs (enqueueCopy) and provider jobs (enqueueProviderCopy) run on
    // entirely separate pipelines; queuing one of each must not block or
    // interfere with the other.
    QTemporaryDir src, dst;
    ASSERT_TRUE(src.isValid() && dst.isValid());
    const QString localSource = writeFile(src.path(), "local.txt", "local-data");
    const QString providerSource = writeFile(src.path(), "provider.txt", "provider-data");

    LocalFileProvider srcProvider;
    LocalFileProvider dstProvider;

    OperationQueue queue;
    QSignalSpy finished(&queue, &OperationQueue::finished);
    queue.enqueueCopy({localSource}, dst.path());
    queue.enqueueProviderCopy(&srcProvider, {providerSource}, &dstProvider, dst.path());

    while (finished.count() < 2 && finished.wait(3000)) {
    }
    ASSERT_EQ(finished.count(), 2);
    EXPECT_TRUE(QFile::exists(QDir(dst.path()).filePath("local.txt")));
    EXPECT_TRUE(QFile::exists(QDir(dst.path()).filePath("provider.txt")));
}
