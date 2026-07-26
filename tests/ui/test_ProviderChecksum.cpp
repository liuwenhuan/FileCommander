#include <gtest/gtest.h>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QVector>

#include <zlib.h>

#include <atomic>
#include <cstring>
#include <memory>

#include "FileInfo.h"
#include "FileProvider.h"
#include "dialogs/ChecksumDialog.h"

// Checksums of files on a share (or inside an archive).
//
// The bug these guard: "Calculate Checksums" filtered the selection with
// QFileInfo::isFile(), which is false for every path a network backend hands
// out, so the user was told they had selected nothing. Worse, when a local file
// happened to sit at the same path, that file's bytes were hashed and the result
// was labelled with the remote file's name -- wrong data presented as right.
//
// There is no server here, so the tests drive the worker through a fake provider
// serving bytes from memory. That is exactly the surface the real backends
// expose (openRead/read/closeHandle), so only the wire is simulated.
namespace {

struct FakeHandle : public FileHandle {
    qint64 offset = 0;
};

// Serves per-path content through the streaming API. openRead() of a path it
// does not know returns nullptr, which is how a real backend reports a file it
// cannot open.
class FakeShare : public FileProvider {
public:
    void addFile(const QString &path, const QByteArray &content) { m_files.insert(path, content); }

    QVector<FileInfo> list(const QString &, bool) const override { return {}; }
    bool isDir(const QString &) const override { return false; }
    QString cleanPath(const QString &p) const override { return p; }
    QString parentPath(const QString &) const override { return {}; }
    bool exists(const QString &p) const override { return m_files.contains(p); }
    RenameResult rename(const QString &, const QString &, QString *) override {
        return RenameResult::Failed;
    }

    QString displayName() const override { return QStringLiteral("tester@share"); }
    bool canStream() const override { return true; }

    FileHandle *openRead(const QString &path) override {
        if (!m_files.contains(path))
            return nullptr;
        ++m_opens;
        auto *h = new FakeHandle();
        m_open.insert(h, m_files.value(path));
        return h;
    }

    qint64 read(FileHandle *handle, char *buffer, qint64 maxSize) override {
        auto *h = static_cast<FakeHandle *>(handle);
        const QByteArray &content = m_open[h];
        const qint64 n = qMin<qint64>(content.size() - h->offset, maxSize);
        if (n > 0) {
            std::memcpy(buffer, content.constData() + h->offset, static_cast<size_t>(n));
            h->offset += n;
        }
        return n;
    }

    void closeHandle(FileHandle *handle) override {
        m_open.remove(static_cast<FakeHandle *>(handle));
        ++m_closes;
        delete handle;
    }

    int opens() const { return m_opens; }
    int closes() const { return m_closes; }
    int leaked() const { return m_open.size(); }

private:
    QHash<QString, QByteArray> m_files;
    QHash<FakeHandle *, QByteArray> m_open;
    int m_opens = 0;
    int m_closes = 0;
};

QByteArray payload(int size, char seed) {
    QByteArray data;
    data.resize(size);
    for (int i = 0; i < size; ++i)
        data[i] = static_cast<char>((i + seed) % 251);
    return data;
}

QString md5Of(const QByteArray &data) {
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Md5).toHex());
}

QString sha1Of(const QByteArray &data) {
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha1).toHex());
}

QString crc32Of(const QByteArray &data) {
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, reinterpret_cast<const Bytef *>(data.constData()),
                static_cast<uInt>(data.size()));
    return QStringLiteral("%1").arg(static_cast<quint32>(crc), 8, 16, QLatin1Char('0')).toUpper();
}

FileInfo entry(const QString &path, qint64 size, bool isDir = false) {
    return FileInfo::fromFields(path, QFileInfo(path).fileName(), size,
                                QDateTime::currentDateTime(), isDir, QFile::ReadOwner);
}

// Runs the worker on this thread. The worker only communicates through signals,
// and process() is a plain call -- moving it to a QThread is the dialog's job,
// not part of what is under test here.
struct HashRun {
    QVector<QStringList> rows; // row -> {md5, crc32, sha1}
    qint64 lastDone = -1;
    qint64 lastTotal = -1;
};

HashRun runWorker(ChecksumWorker &worker, int rowCount) {
    HashRun out;
    out.rows.resize(rowCount);
    QObject::connect(&worker, &ChecksumWorker::rowReady,
                     [&out](int row, const QString &md5, const QString &crc, const QString &sha1) {
                         if (row >= 0 && row < out.rows.size())
                             out.rows[row] = QStringList{md5, crc, sha1};
                     });
    QObject::connect(&worker, &ChecksumWorker::progress, [&out](qint64 done, qint64 total) {
        out.lastDone = done;
        out.lastTotal = total;
    });
    worker.process();
    return out;
}

} // namespace

TEST(ProviderChecksumTest, HashesTheBytesTheProviderServes) {
    const QByteArray a = payload(3 * 1024 * 1024 + 17, 1); // spans several 1 MiB chunks
    const QByteArray b = payload(64, 9);

    auto share = std::make_shared<FakeShare>();
    share->addFile("/share/a.bin", a);
    share->addFile("/share/b.bin", b);

    auto cancel = std::make_shared<std::atomic<bool>>(false);
    ChecksumWorker worker(QVector<FileInfo>{entry("/share/a.bin", a.size()),
                                            entry("/share/b.bin", b.size())},
                          share, cancel);
    const HashRun out = runWorker(worker, 2);

    ASSERT_EQ(out.rows.at(0).size(), 3);
    EXPECT_EQ(out.rows.at(0).at(0), md5Of(a));
    EXPECT_EQ(out.rows.at(0).at(1), crc32Of(a));
    EXPECT_EQ(out.rows.at(0).at(2), sha1Of(a));

    ASSERT_EQ(out.rows.at(1).size(), 3);
    EXPECT_EQ(out.rows.at(1).at(0), md5Of(b));
    EXPECT_EQ(out.rows.at(1).at(1), crc32Of(b));
    EXPECT_EQ(out.rows.at(1).at(2), sha1Of(b));

    // Progress ends at the byte total taken from the listing, and every handle
    // that was opened was handed back.
    EXPECT_EQ(out.lastTotal, static_cast<qint64>(a.size() + b.size()));
    EXPECT_EQ(out.lastDone, out.lastTotal);
    EXPECT_EQ(share->opens(), 2);
    EXPECT_EQ(share->closes(), 2);
    EXPECT_EQ(share->leaked(), 0);
}

TEST(ProviderChecksumTest, IgnoresALocalFileThatSharesTheRemotePath) {
    // The dangerous case: /tmp/.../notes.txt exists here AND on the server, with
    // different contents. Hashing the local one and labelling it with the remote
    // name is wrong data presented as a correct answer.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = QDir(dir.path()).filePath("notes.txt");
    QFile local(path);
    ASSERT_TRUE(local.open(QIODevice::WriteOnly));
    local.write("these are the LOCAL bytes");
    local.close();
    ASSERT_TRUE(QFileInfo(path).isFile());

    const QByteArray remote("these are the REMOTE bytes");
    auto share = std::make_shared<FakeShare>();
    share->addFile(path, remote);

    auto cancel = std::make_shared<std::atomic<bool>>(false);
    ChecksumWorker worker(QVector<FileInfo>{entry(path, remote.size())}, share, cancel);
    const HashRun out = runWorker(worker, 1);

    ASSERT_EQ(out.rows.at(0).size(), 3);
    EXPECT_EQ(out.rows.at(0).at(0), md5Of(remote));
    EXPECT_NE(out.rows.at(0).at(0), md5Of(QByteArray("these are the LOCAL bytes")));
}

TEST(ProviderChecksumTest, DirectoriesAndUnopenableEntriesAreReportedNotHashed) {
    const QByteArray ok = payload(128, 3);
    auto share = std::make_shared<FakeShare>();
    share->addFile("/share/ok.bin", ok);
    // "/share/gone.bin" is deliberately not served: openRead returns nullptr.

    auto cancel = std::make_shared<std::atomic<bool>>(false);
    ChecksumWorker worker(QVector<FileInfo>{entry("/share/sub", 4096, /*isDir=*/true),
                                            entry("/share/gone.bin", 10),
                                            entry("/share/ok.bin", ok.size())},
                          share, cancel);
    const HashRun out = runWorker(worker, 3);

    EXPECT_FALSE(out.rows.at(0).at(0).isEmpty());
    EXPECT_EQ(out.rows.at(0).at(0), out.rows.at(0).at(2)); // the same "(directory)" marker
    EXPECT_FALSE(out.rows.at(1).at(0).isEmpty());
    EXPECT_EQ(out.rows.at(1).at(0), out.rows.at(1).at(2)); // "(unreadable)"
    EXPECT_EQ(out.rows.at(2).at(0), md5Of(ok));

    // A directory is never opened, and the entry that could not be opened has
    // nothing to hand back.
    EXPECT_EQ(share->opens(), 1);
    EXPECT_EQ(share->leaked(), 0);
}

TEST(ProviderChecksumTest, CancellationStopsBeforeTheNextEntry) {
    const QByteArray a = payload(4096, 5);
    auto share = std::make_shared<FakeShare>();
    share->addFile("/share/a.bin", a);
    share->addFile("/share/b.bin", a);

    auto cancel = std::make_shared<std::atomic<bool>>(true); // closed before it began
    ChecksumWorker worker(QVector<FileInfo>{entry("/share/a.bin", a.size()),
                                            entry("/share/b.bin", a.size())},
                          share, cancel);
    const HashRun out = runWorker(worker, 2);

    EXPECT_TRUE(out.rows.at(0).isEmpty());
    EXPECT_EQ(share->opens(), 0);
}

// The local path is untouched by all of the above: same worker, no provider.
TEST(ProviderChecksumTest, LocalBatchStillReadsTheLocalFilesystem) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = QDir(dir.path()).filePath("local.bin");
    const QByteArray content = payload(5000, 7);
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(content);
    f.close();

    auto cancel = std::make_shared<std::atomic<bool>>(false);
    ChecksumWorker worker(QStringList{path}, cancel);
    const HashRun out = runWorker(worker, 1);

    ASSERT_EQ(out.rows.at(0).size(), 3);
    EXPECT_EQ(out.rows.at(0).at(0), md5Of(content));
    EXPECT_EQ(out.rows.at(0).at(1), crc32Of(content));
    EXPECT_EQ(out.rows.at(0).at(2), sha1Of(content));
    EXPECT_EQ(out.lastTotal, static_cast<qint64>(content.size()));
}
