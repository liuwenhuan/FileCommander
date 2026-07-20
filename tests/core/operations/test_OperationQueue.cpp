#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QSignalSpy>

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
    queue.setConflictHandler([&prompts](const QString &, const QString &) {
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
    QTemporaryDir src, dst;
    ASSERT_TRUE(src.isValid() && dst.isValid());
    const QString source = writeFile(src.path(), "t.txt", "linked");

    OperationQueue queue;
    QSignalSpy finished(&queue, &OperationQueue::finished);
    queue.enqueueSymlink({source}, dst.path());

    ASSERT_TRUE(waitFinished(finished));
    EXPECT_TRUE(QFileInfo(QDir(dst.path()).filePath("t.txt")).isSymLink());
}
