#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include "ArchiveHandler.h"

// The layout rules behind "Extract Here". They decide where a user's files end
// up, and until now nothing asserted any of them -- the code was written, then
// left unreachable because no menu ever called it, so no test ever noticed.
namespace {

QString writeFile(const QString &dir, const QString &relPath, const QByteArray &content) {
    const QString path = QDir(dir).filePath(relPath);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    file.write(content);
    file.close();
    return path;
}

// Packs `sources` (paths, each added at the archive root under its basename)
// into <dir>/<name>.zip. Returns an empty string when the format is unavailable,
// so a caller can skip rather than fail.
QString makeZip(const QString &dir, const QString &name, const QStringList &sources) {
    const QString path = QDir(dir).filePath(name + QStringLiteral(".zip"));
    QString err;
    if (!ArchiveHandler::create(path, sources, QStringLiteral("zip"), &err))
        return {};
    return path;
}

} // namespace

// Several top-level entries would otherwise spray across the directory the user
// was standing in, mixed with what was already there and with no way to tell
// which files were new. They go into a folder named after the archive.
TEST(SmartExtractTest, SeveralTopLevelEntriesAreWrappedInAnArchiveNamedFolder) {
    QTemporaryDir work;
    ASSERT_TRUE(work.isValid());
    const QString src = QDir(work.path()).filePath(QStringLiteral("src"));
    ASSERT_FALSE(writeFile(src, QStringLiteral("a.txt"), "a").isEmpty());
    ASSERT_FALSE(writeFile(src, QStringLiteral("b.txt"), "b").isEmpty());

    const QString zip = makeZip(work.path(), QStringLiteral("many"),
                                {QDir(src).filePath(QStringLiteral("a.txt")),
                                 QDir(src).filePath(QStringLiteral("b.txt"))});
    if (zip.isEmpty())
        GTEST_SKIP() << "this build cannot write zip archives";

    const QString dest = QDir(work.path()).filePath(QStringLiteral("dest"));
    ASSERT_TRUE(QDir().mkpath(dest));

    QString err;
    const ArchiveHandler::SmartResult res = ArchiveHandler::smartExtract(zip, dest, &err);
    ASSERT_TRUE(res.ok) << qPrintable(err);

    const QString wrapper = QDir(dest).filePath(QStringLiteral("many"));
    EXPECT_EQ(res.finalDir, wrapper);
    EXPECT_TRUE(QFileInfo::exists(QDir(wrapper).filePath(QStringLiteral("a.txt"))));
    EXPECT_TRUE(QFileInfo::exists(QDir(wrapper).filePath(QStringLiteral("b.txt"))));
    // And nothing was dropped beside the wrapper.
    EXPECT_FALSE(QFileInfo::exists(QDir(dest).filePath(QStringLiteral("a.txt"))));
}

// One top-level folder is already a tidy container, so wrapping it would make
// many/many/ -- the "double folder" every archiver gets complained about.
TEST(SmartExtractTest, ASingleTopLevelFolderIsNotWrappedAgain) {
    QTemporaryDir work;
    ASSERT_TRUE(work.isValid());
    const QString src = QDir(work.path()).filePath(QStringLiteral("src"));
    ASSERT_FALSE(writeFile(src, QStringLiteral("payload/one.txt"), "1").isEmpty());
    ASSERT_FALSE(writeFile(src, QStringLiteral("payload/two.txt"), "2").isEmpty());

    const QString zip = makeZip(work.path(), QStringLiteral("single"),
                                {QDir(src).filePath(QStringLiteral("payload"))});
    if (zip.isEmpty())
        GTEST_SKIP() << "this build cannot write zip archives";

    const QString dest = QDir(work.path()).filePath(QStringLiteral("dest"));
    ASSERT_TRUE(QDir().mkpath(dest));

    QString err;
    const ArchiveHandler::SmartResult res = ArchiveHandler::smartExtract(zip, dest, &err);
    ASSERT_TRUE(res.ok) << qPrintable(err);

    EXPECT_EQ(res.finalDir, dest) << "the archive's own folder was wrapped in a second one";
    EXPECT_TRUE(QFileInfo::exists(QDir(dest).filePath(QStringLiteral("payload/one.txt"))));
    EXPECT_FALSE(QFileInfo::exists(QDir(dest).filePath(QStringLiteral("single/payload"))));
}

// Extracting twice must not overwrite the first result. This is the property
// that makes "Extract Here" safe to click without reading the destination
// first: the worst case is a spare folder, never a lost file.
TEST(SmartExtractTest, ASecondExtractionGetsItsOwnFolderInsteadOfOverwriting) {
    QTemporaryDir work;
    ASSERT_TRUE(work.isValid());
    const QString src = QDir(work.path()).filePath(QStringLiteral("src"));
    ASSERT_FALSE(writeFile(src, QStringLiteral("a.txt"), "first").isEmpty());
    ASSERT_FALSE(writeFile(src, QStringLiteral("b.txt"), "second").isEmpty());

    const QString zip = makeZip(work.path(), QStringLiteral("twice"),
                                {QDir(src).filePath(QStringLiteral("a.txt")),
                                 QDir(src).filePath(QStringLiteral("b.txt"))});
    if (zip.isEmpty())
        GTEST_SKIP() << "this build cannot write zip archives";

    const QString dest = QDir(work.path()).filePath(QStringLiteral("dest"));
    ASSERT_TRUE(QDir().mkpath(dest));

    QString err;
    const ArchiveHandler::SmartResult first = ArchiveHandler::smartExtract(zip, dest, &err);
    ASSERT_TRUE(first.ok) << qPrintable(err);
    // Mark the first result so an overwrite would be visible.
    ASSERT_FALSE(writeFile(first.finalDir, QStringLiteral("mine.txt"), "keep me").isEmpty());

    const ArchiveHandler::SmartResult second = ArchiveHandler::smartExtract(zip, dest, &err);
    ASSERT_TRUE(second.ok) << qPrintable(err);

    EXPECT_NE(second.finalDir, first.finalDir);
    EXPECT_TRUE(QFileInfo::exists(QDir(first.finalDir).filePath(QStringLiteral("mine.txt"))))
        << "the second extraction wrote over the first";
}
