#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <memory>

#include "FileInfo.h"
#include "FileProvider.h"
#include "SyncScanner.h"

namespace {

void writeFile(const QString &dir, const QString &relPath, const QByteArray &content) {
    const QString path = QDir(dir).filePath(relPath);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write(content);
    file.close();
}

// Runs a scanner to completion on the calling thread and returns everything it
// emitted, flattened in emission order.
QVector<SyncEntry> runScan(SyncScanner *scanner) {
    QVector<SyncEntry> all;
    QObject::connect(scanner, &SyncScanner::entriesReady,
                     [&all](quint64, const QVector<SyncEntry> &batch) { all.append(batch); });
    scanner->process();
    return all;
}

const SyncEntry *findEntry(const QVector<SyncEntry> &entries, const QString &relPath) {
    for (const auto &e : entries) {
        if (e.relativePath == relPath)
            return &e;
    }
    return nullptr;
}

// An in-memory provider standing in for a remote backend, so the provider code
// path can be exercised without a server. Shaped after test_NetworkSession.cpp's
// StubProvider.
class MemoryProvider : public FileProvider {
public:
    // Adds a file at an absolute path like "/root/sub/file.txt".
    void addFile(const QString &path, qint64 size, const QDateTime &modified) {
        m_files.insert(path, qMakePair(size, modified));
        QString parent = path.section(QLatin1Char('/'), 0, -2);
        while (!parent.isEmpty()) {
            m_dirs.insert(parent);
            parent = parent.section(QLatin1Char('/'), 0, -2);
        }
    }

    int listCalls() const { return m_listCalls; }
    // Highest number of list() calls observed running at the same time. Anything
    // above 1 would mean the scanner drove the backend concurrently.
    int peakConcurrentLists() const { return m_peakConcurrent; }

    QVector<FileInfo> list(const QString &path, bool) const override {
        const int now = ++m_concurrent;
        if (now > m_peakConcurrent)
            m_peakConcurrent = now;
        ++m_listCalls;
        // Hold the "call" open briefly so a genuinely concurrent caller would
        // overlap with us and be caught by the counter above.
        QThread::msleep(2);

        QVector<FileInfo> out;
        const QString prefix = path.endsWith(QLatin1Char('/')) ? path : path + QLatin1Char('/');
        QSet<QString> seenDirs;
        for (auto it = m_files.constBegin(); it != m_files.constEnd(); ++it) {
            if (!it.key().startsWith(prefix))
                continue;
            const QString rest = it.key().mid(prefix.size());
            if (rest.contains(QLatin1Char('/'))) {
                const QString child = rest.section(QLatin1Char('/'), 0, 0);
                if (!seenDirs.contains(child)) {
                    seenDirs.insert(child);
                    out.append(FileInfo::fromFields(prefix + child, child, 0, QDateTime(),
                                                     /*isDir=*/true, QFile::Permissions()));
                }
            } else {
                out.append(FileInfo::fromFields(it.key(), rest, it.value().first, it.value().second,
                                                 /*isDir=*/false, QFile::Permissions()));
            }
        }
        --m_concurrent;
        return out;
    }

    bool isDir(const QString &path) const override { return m_dirs.contains(path); }
    QString cleanPath(const QString &p) const override { return p; }
    QString parentPath(const QString &p) const override {
        return p.section(QLatin1Char('/'), 0, -2);
    }
    bool exists(const QString &path) const override {
        return m_files.contains(path) || m_dirs.contains(path);
    }
    RenameResult rename(const QString &, const QString &, QString *) override {
        return RenameResult::Failed;
    }

private:
    QMap<QString, QPair<qint64, QDateTime>> m_files;
    QSet<QString> m_dirs;
    mutable int m_listCalls = 0;
    mutable std::atomic<int> m_concurrent{0};
    mutable int m_peakConcurrent = 0;
};

} // namespace

TEST(SyncScannerTest, ClassifiesLocalTreeLikeDirectorySync) {
    QTemporaryDir left, right;
    ASSERT_TRUE(left.isValid() && right.isValid());
    writeFile(left.path(), "only_left.txt", "a");
    writeFile(right.path(), "only_right.txt", "b");
    writeFile(left.path(), "same.txt", "identical");
    writeFile(right.path(), "same.txt", "identical");

    auto cancel = std::make_shared<std::atomic<bool>>(false);
    SyncScanner scanner(left.path(), nullptr, right.path(), nullptr, /*recursive=*/true, cancel);
    const auto entries = runScan(&scanner);

    ASSERT_EQ(entries.size(), 3);
    EXPECT_EQ(findEntry(entries, "only_left.txt")->status, SyncEntry::Status::LeftOnly);
    EXPECT_EQ(findEntry(entries, "only_right.txt")->status, SyncEntry::Status::RightOnly);
    EXPECT_EQ(findEntry(entries, "same.txt")->status, SyncEntry::Status::Same);
}

TEST(SyncScannerTest, RecursiveFlagControlsDescent) {
    QTemporaryDir left, right;
    ASSERT_TRUE(left.isValid() && right.isValid());
    writeFile(left.path(), "sub/dir/nested.txt", "nested");

    auto cancel = std::make_shared<std::atomic<bool>>(false);
    SyncScanner shallow(left.path(), nullptr, right.path(), nullptr, /*recursive=*/false, cancel);
    EXPECT_TRUE(runScan(&shallow).isEmpty());

    SyncScanner deep(left.path(), nullptr, right.path(), nullptr, /*recursive=*/true, cancel);
    const auto entries = runScan(&deep);
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries.first().relativePath, QString("sub/dir/nested.txt"));
    EXPECT_EQ(entries.first().status, SyncEntry::Status::LeftOnly);
}

TEST(SyncScannerTest, DescendsDirectoriesPresentOnOneSideOnly) {
    QTemporaryDir left, right;
    ASSERT_TRUE(left.isValid() && right.isValid());
    // "extra/" exists only on the left; its contents must still be reported.
    writeFile(left.path(), "extra/a.txt", "a");
    writeFile(left.path(), "extra/deeper/b.txt", "b");

    auto cancel = std::make_shared<std::atomic<bool>>(false);
    SyncScanner scanner(left.path(), nullptr, right.path(), nullptr, true, cancel);
    const auto entries = runScan(&scanner);

    ASSERT_EQ(entries.size(), 2);
    EXPECT_EQ(findEntry(entries, "extra/a.txt")->status, SyncEntry::Status::LeftOnly);
    EXPECT_EQ(findEntry(entries, "extra/deeper/b.txt")->status, SyncEntry::Status::LeftOnly);
}

TEST(SyncScannerTest, ResultsStreamInMultipleBatches) {
    QTemporaryDir left, right;
    ASSERT_TRUE(left.isValid() && right.isValid());
    // More than one batch worth, so the walk must flush before it finishes.
    const int fileCount = SyncScanner::kBatchSize * 2 + 25;
    for (int i = 0; i < fileCount; ++i)
        writeFile(left.path(), QStringLiteral("f%1.txt").arg(i, 4, 10, QLatin1Char('0')), "x");

    auto cancel = std::make_shared<std::atomic<bool>>(false);
    SyncScanner scanner(left.path(), nullptr, right.path(), nullptr, true, cancel);

    int batches = 0;
    QVector<SyncEntry> all;
    QObject::connect(&scanner, &SyncScanner::entriesReady,
                     [&](quint64, const QVector<SyncEntry> &batch) {
                         ++batches;
                         all.append(batch);
                     });
    scanner.process();

    EXPECT_EQ(all.size(), fileCount);
    // The whole point of the rework: results arrive progressively, not in one lump.
    EXPECT_GT(batches, 1);
}

TEST(SyncScannerTest, CancelStopsTheWalkAndReportsCancelled) {
    QTemporaryDir left, right;
    ASSERT_TRUE(left.isValid() && right.isValid());
    const int fileCount = SyncScanner::kBatchSize * 4;
    for (int i = 0; i < fileCount; ++i)
        writeFile(left.path(), QStringLiteral("f%1.txt").arg(i, 4, 10, QLatin1Char('0')), "x");

    auto cancel = std::make_shared<std::atomic<bool>>(false);
    SyncScanner scanner(left.path(), nullptr, right.path(), nullptr, true, cancel);

    QVector<SyncEntry> all;
    int batches = 0;
    QObject::connect(&scanner, &SyncScanner::entriesReady,
                     [&](quint64, const QVector<SyncEntry> &batch) {
                         ++batches;
                         all.append(batch);
                         // Stop as soon as the first batch lands.
                         cancel->store(true);
                     });
    QSignalSpy finishedSpy(&scanner, &SyncScanner::finished);
    scanner.process();

    ASSERT_EQ(finishedSpy.count(), 1);
    EXPECT_TRUE(finishedSpy.first().at(1).toBool()); // finished(scanId, cancelled=true)
    // It stopped early rather than walking the whole tree, and emitted nothing
    // more after the cancel was seen.
    EXPECT_EQ(batches, 1);
    EXPECT_LT(all.size(), fileCount);
}

TEST(SyncScannerTest, CompletedScanReportsNotCancelled) {
    QTemporaryDir left, right;
    ASSERT_TRUE(left.isValid() && right.isValid());
    writeFile(left.path(), "a.txt", "a");

    auto cancel = std::make_shared<std::atomic<bool>>(false);
    SyncScanner scanner(left.path(), nullptr, right.path(), nullptr, true, cancel);
    QSignalSpy finishedSpy(&scanner, &SyncScanner::finished);
    scanner.process();

    ASSERT_EQ(finishedSpy.count(), 1);
    EXPECT_FALSE(finishedSpy.first().at(1).toBool());
}

TEST(SyncScannerTest, ProviderPathMatchesLocalPath) {
    // The same tree described twice -- once on disk, once in a fake provider --
    // must classify identically, so remote comparison isn't a second code path
    // with its own behaviour.
    QTemporaryDir left, right;
    ASSERT_TRUE(left.isValid() && right.isValid());
    const QDateTime t1 = QDateTime::fromSecsSinceEpoch(1700000000);
    const QDateTime t2 = QDateTime::fromSecsSinceEpoch(1700009999);

    MemoryProvider leftProv, rightProv;
    leftProv.addFile("/L/only_left.txt", 1, t1);
    leftProv.addFile("/L/both.txt", 10, t1);
    leftProv.addFile("/L/sub/nested.txt", 5, t1);
    rightProv.addFile("/R/only_right.txt", 2, t1);
    rightProv.addFile("/R/both.txt", 10, t1);
    rightProv.addFile("/R/sub/nested.txt", 5, t2); // newer on the right

    auto cancel = std::make_shared<std::atomic<bool>>(false);
    SyncScanner scanner("/L", &leftProv, "/R", &rightProv, /*recursive=*/true, cancel);
    const auto entries = runScan(&scanner);

    ASSERT_EQ(entries.size(), 4);
    EXPECT_EQ(findEntry(entries, "only_left.txt")->status, SyncEntry::Status::LeftOnly);
    EXPECT_EQ(findEntry(entries, "only_right.txt")->status, SyncEntry::Status::RightOnly);
    EXPECT_EQ(findEntry(entries, "both.txt")->status, SyncEntry::Status::Same);
    EXPECT_EQ(findEntry(entries, "sub/nested.txt")->status, SyncEntry::Status::Different);
}

TEST(SyncScannerTest, NeverReadsTheTwoSidesConcurrently) {
    // libsmbclient cannot be driven concurrently in-process (commit 3a9f440), so
    // the walk must stay strictly serial. A single shared provider standing in
    // for both sides makes any overlap observable.
    MemoryProvider shared;
    const QDateTime t = QDateTime::fromSecsSinceEpoch(1700000000);
    for (int i = 0; i < 12; ++i) {
        shared.addFile(QStringLiteral("/L/sub%1/f.txt").arg(i), 1, t);
        shared.addFile(QStringLiteral("/R/sub%1/f.txt").arg(i), 1, t);
    }

    auto cancel = std::make_shared<std::atomic<bool>>(false);
    SyncScanner scanner("/L", &shared, "/R", &shared, /*recursive=*/true, cancel);
    runScan(&scanner);

    EXPECT_GT(shared.listCalls(), 2);
    EXPECT_EQ(shared.peakConcurrentLists(), 1);
}

TEST(SyncScannerTest, EverySignalCarriesTheScanId) {
    // The owner tells one run's results from another's by this id. Without it, a
    // restarted comparison appended the old run's in-flight batches on top of the
    // new run's rows and every entry appeared twice.
    QTemporaryDir left, right;
    ASSERT_TRUE(left.isValid() && right.isValid());
    writeFile(left.path(), "a.txt", "a");

    auto cancel = std::make_shared<std::atomic<bool>>(false);
    const quint64 kId = 42;
    SyncScanner scanner(left.path(), nullptr, right.path(), nullptr, true, cancel, kId);

    QVector<quint64> entryIds, progressIds;
    QObject::connect(&scanner, &SyncScanner::entriesReady,
                     [&](quint64 id, const QVector<SyncEntry> &) { entryIds.append(id); });
    QObject::connect(&scanner, &SyncScanner::progress,
                     [&](quint64 id, int, const QString &) { progressIds.append(id); });
    QSignalSpy finishedSpy(&scanner, &SyncScanner::finished);
    scanner.process();

    ASSERT_FALSE(entryIds.isEmpty());
    for (quint64 id : entryIds)
        EXPECT_EQ(id, kId);
    ASSERT_FALSE(progressIds.isEmpty());
    for (quint64 id : progressIds)
        EXPECT_EQ(id, kId);
    ASSERT_EQ(finishedSpy.count(), 1);
    EXPECT_EQ(finishedSpy.first().at(0).toULongLong(), kId);
}

TEST(SyncScannerTest, TimeToleranceBoundary) {
    const QDateTime base = QDateTime::fromSecsSinceEpoch(1700000000);
    const qint64 tol = DirectorySync::kTimeToleranceMs;

    // Exactly at the tolerance still counts as the same instant...
    EXPECT_EQ(DirectorySync::classify(100, base, true, 100, base.addMSecs(tol), true),
              SyncEntry::Status::Same);
    // ...one millisecond past it does not.
    EXPECT_EQ(DirectorySync::classify(100, base, true, 100, base.addMSecs(tol + 1), true),
              SyncEntry::Status::Different);
    // Symmetric in sign.
    EXPECT_EQ(DirectorySync::classify(100, base, true, 100, base.addMSecs(-(tol + 1)), true),
              SyncEntry::Status::Different);
    // A size difference alone is enough, even with identical timestamps.
    EXPECT_EQ(DirectorySync::classify(100, base, true, 101, base, true),
              SyncEntry::Status::Different);
}
