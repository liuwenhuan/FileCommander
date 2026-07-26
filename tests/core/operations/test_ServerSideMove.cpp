#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "FileOperations.h"
#include "LocalFileProvider.h"

// Server-side move fast path: when a move stays inside one backend,
// copyAcrossProviders first asks the provider to relocate the entry on the
// server instead of streaming every byte through the client (measured at 191x
// on a real SMB share). These tests cover the dispatch of the provider's
// four-way answer, the fall-back guarantees, and the per-batch suppression.
//
// A real server is not available in CI, so the backend here is a
// LocalFileProvider whose moveTo() is scripted -- the transfer engine's
// behaviour is what is under test, not any one protocol's implementation.
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

// A local provider with a programmable moveTo(). Everything else (listing,
// streaming, remove) is the genuine local implementation, so when the fast path
// declines, the fall-back exercised is the real streaming code.
class ScriptedProvider : public LocalFileProvider {
public:
    // What moveTo() should answer. Real moves only happen for Ok.
    FileProvider::RenameResult answer = FileProvider::RenameResult::Unsupported;
    int calls = 0;

    RenameResult moveTo(const QString &srcPath, const QString &dstPath) override {
        ++calls;
        if (answer != RenameResult::Ok)
            return answer; // declined: the source must stay untouched
        return QDir().rename(srcPath, dstPath) ? RenameResult::Ok : RenameResult::Failed;
    }
};

} // namespace

TEST(ServerSideMoveTest, UsesFastPathAndLeavesNoStreamedCopy) {
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const QByteArray payload("relocated without touching the wire");
    const QString source = writeFile(srcDir.path(), "fast.txt", payload);

    ScriptedProvider provider;
    provider.answer = FileProvider::RenameResult::Ok;
    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.copyAcrossProviders(&provider, {source}, &provider, dstDir.path(),
                                        /*removeSource=*/true, nullptr, &err))
        << err.toStdString();

    EXPECT_EQ(provider.calls, 1);
    EXPECT_FALSE(QFile::exists(source));
    EXPECT_EQ(readFile(QDir(dstDir.path()).filePath("fast.txt")), payload);
}

// The headline guarantee: a backend that cannot move server-side must produce
// exactly the same result as before the fast path existed.
TEST(ServerSideMoveTest, FallsBackToStreamingWhenUnsupported) {
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const QByteArray payload("streamed the long way round");
    const QString source = writeFile(srcDir.path(), "slow.txt", payload);

    ScriptedProvider provider;
    provider.answer = FileProvider::RenameResult::Unsupported;
    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.copyAcrossProviders(&provider, {source}, &provider, dstDir.path(),
                                        /*removeSource=*/true, nullptr, &err))
        << err.toStdString();

    EXPECT_TRUE(err.isEmpty()); // declining is not an error the user should see
    EXPECT_FALSE(QFile::exists(source));
    EXPECT_EQ(readFile(QDir(dstDir.path()).filePath("slow.txt")), payload);
}

// Failed is not treated as fatal either: a backend cannot always tell "can't"
// from "didn't" (SFTP returns one code for both), so it must also fall back.
TEST(ServerSideMoveTest, FallsBackToStreamingWhenFailed) {
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const QByteArray payload("failed fast, copied anyway");
    const QString source = writeFile(srcDir.path(), "failed.txt", payload);

    ScriptedProvider provider;
    provider.answer = FileProvider::RenameResult::Failed;
    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.copyAcrossProviders(&provider, {source}, &provider, dstDir.path(),
                                        /*removeSource=*/true, nullptr, &err))
        << err.toStdString();

    EXPECT_TRUE(err.isEmpty());
    EXPECT_FALSE(QFile::exists(source));
    EXPECT_EQ(readFile(QDir(dstDir.path()).filePath("failed.txt")), payload);
}

// An unsupported backend must be asked once, not once per entry: on SMB a
// cross-share move is refused identically for every file, and a large directory
// should not pay a round trip per entry to be told so again.
TEST(ServerSideMoveTest, StopsAskingAfterFirstUnsupported) {
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    QStringList sources;
    for (int i = 0; i < 5; ++i)
        sources << writeFile(srcDir.path(), QStringLiteral("f%1.txt").arg(i), QByteArray("x"));

    ScriptedProvider provider;
    provider.answer = FileProvider::RenameResult::Unsupported;
    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.copyAcrossProviders(&provider, sources, &provider, dstDir.path(),
                                        /*removeSource=*/true, nullptr, &err))
        << err.toStdString();

    EXPECT_EQ(provider.calls, 1); // asked once for the batch, never again
    for (int i = 0; i < 5; ++i)
        EXPECT_TRUE(QFile::exists(QDir(dstDir.path()).filePath(QStringLiteral("f%1.txt").arg(i))));
}

// An occupied destination says nothing about the rest of the batch, so it must
// not switch the fast path off the way Unsupported does.
TEST(ServerSideMoveTest, KeepsTryingAfterOccupiedDestination) {
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    QStringList sources;
    for (int i = 0; i < 3; ++i)
        sources << writeFile(srcDir.path(), QStringLiteral("g%1.txt").arg(i), QByteArray("y"));

    ScriptedProvider provider;
    provider.answer = FileProvider::RenameResult::AlreadyExists;
    FileOperations ops;
    QString err;
    // No resolver: an unresolved conflict skips the entry rather than failing.
    ASSERT_TRUE(ops.copyAcrossProviders(&provider, sources, &provider, dstDir.path(),
                                        /*removeSource=*/true, nullptr, &err))
        << err.toStdString();

    EXPECT_EQ(provider.calls, 3); // still offered every entry
}

// A copy is not a move: nothing may be relocated on the server when the source
// is meant to survive.
TEST(ServerSideMoveTest, NeverUsedForCopy) {
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const QByteArray payload("copies must keep the original");
    const QString source = writeFile(srcDir.path(), "copy.txt", payload);

    ScriptedProvider provider;
    provider.answer = FileProvider::RenameResult::Ok;
    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.copyAcrossProviders(&provider, {source}, &provider, dstDir.path(),
                                        /*removeSource=*/false, nullptr, &err))
        << err.toStdString();

    EXPECT_EQ(provider.calls, 0);
    EXPECT_TRUE(QFile::exists(source));
    EXPECT_EQ(readFile(QDir(dstDir.path()).filePath("copy.txt")), payload);
}

// The fast path is strictly intra-backend: relocating on one server cannot
// deliver bytes to a different one.
TEST(ServerSideMoveTest, NeverUsedAcrossDifferentProviders) {
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const QString source = writeFile(srcDir.path(), "cross.txt", QByteArray("two backends"));

    ScriptedProvider from, to;
    from.answer = FileProvider::RenameResult::Ok;
    to.answer = FileProvider::RenameResult::Ok;
    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.copyAcrossProviders(&from, {source}, &to, dstDir.path(),
                                        /*removeSource=*/true, nullptr, &err))
        << err.toStdString();

    EXPECT_EQ(from.calls, 0);
    EXPECT_EQ(to.calls, 0);
    EXPECT_FALSE(QFile::exists(source));
}

// A directory relocates in a single call rather than being walked entry by
// entry -- the case where the saving is largest.
TEST(ServerSideMoveTest, MovesWholeDirectoryInOneCall) {
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    QDir(srcDir.path()).mkdir("tree");
    const QString treeRoot = QDir(srcDir.path()).filePath("tree");
    QDir(treeRoot).mkdir("nested");
    writeFile(treeRoot, "top.txt", QByteArray("top"));
    writeFile(QDir(treeRoot).filePath("nested"), "inner.txt", QByteArray("inner"));

    ScriptedProvider provider;
    provider.answer = FileProvider::RenameResult::Ok;
    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.copyAcrossProviders(&provider, {treeRoot}, &provider, dstDir.path(),
                                        /*removeSource=*/true, nullptr, &err))
        << err.toStdString();

    EXPECT_EQ(provider.calls, 1); // one call for the whole tree, not per file
    EXPECT_FALSE(QDir(treeRoot).exists());
    EXPECT_EQ(readFile(QDir(dstDir.path()).filePath("tree/top.txt")), QByteArray("top"));
    EXPECT_EQ(readFile(QDir(dstDir.path()).filePath("tree/nested/inner.txt")),
              QByteArray("inner"));
}

// Progress must not stall at 0% just because no bytes crossed the wire.
TEST(ServerSideMoveTest, CreditsBytesForServerSideMove) {
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const QByteArray payload(4096, 'z');
    const QString source = writeFile(srcDir.path(), "bytes.bin", payload);

    ScriptedProvider provider;
    provider.answer = FileProvider::RenameResult::Ok;
    FileOperations ops;
    qint64 lastDoneBytes = -1;
    qint64 lastTotalBytes = -1;
    QObject::connect(&ops, &FileOperations::progress,
                     [&](qint64, qint64, qint64 doneBytes, qint64 totalBytes, const QString &) {
                         lastDoneBytes = doneBytes;
                         lastTotalBytes = totalBytes;
                     });
    QString err;
    ASSERT_TRUE(ops.copyAcrossProviders(&provider, {source}, &provider, dstDir.path(),
                                        /*removeSource=*/true, nullptr, &err))
        << err.toStdString();

    EXPECT_EQ(lastTotalBytes, payload.size());
    EXPECT_EQ(lastDoneBytes, payload.size()); // reaches 100%, not stuck at 0
}
