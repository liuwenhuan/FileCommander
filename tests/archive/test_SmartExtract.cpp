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

// A wrapper holding exactly one archive reports where the inner one landed.
// That report is what drives the recursion: MainWindow feeds it back in until
// nothing is left packed, so it has to be a path that actually exists.
TEST(SmartExtractTest, AResultThatIsOneArchiveReportsTheInnerPath) {
    QTemporaryDir work;
    ASSERT_TRUE(work.isValid());
    const QString src = QDir(work.path()).filePath(QStringLiteral("src"));
    ASSERT_FALSE(writeFile(src, QStringLiteral("deep.txt"), "buried").isEmpty());

    const QString inner = makeZip(src, QStringLiteral("inner"),
                                  {QDir(src).filePath(QStringLiteral("deep.txt"))});
    if (inner.isEmpty())
        GTEST_SKIP() << "this build cannot write zip archives";
    const QString outer = makeZip(work.path(), QStringLiteral("outer"), {inner});
    ASSERT_FALSE(outer.isEmpty());

    const QString dest = QDir(work.path()).filePath(QStringLiteral("dest"));
    ASSERT_TRUE(QDir().mkpath(dest));

    QString err;
    const ArchiveHandler::SmartResult res = ArchiveHandler::smartExtract(outer, dest, &err);
    ASSERT_TRUE(res.ok) << qPrintable(err);
    ASSERT_FALSE(res.nestedArchivePath.isEmpty()) << "the inner archive went unreported";
    EXPECT_TRUE(QFileInfo::exists(res.nestedArchivePath))
        << "reported a path that is not there: " << qPrintable(res.nestedArchivePath);

    // And feeding it back in -- what the recursion does -- reaches the payload.
    const ArchiveHandler::SmartResult second = ArchiveHandler::smartExtract(
        res.nestedArchivePath, QFileInfo(res.nestedArchivePath).absolutePath(), &err);
    ASSERT_TRUE(second.ok) << qPrintable(err);
    EXPECT_TRUE(QFileInfo::exists(QDir(second.finalDir).filePath(QStringLiteral("deep.txt"))));
}

// Encryption is reported while listing, BEFORE anything is written. That
// ordering is what makes an automatic recursion safe to prompt from: the
// caller can ask for a password and retry the same archive without having to
// undo a half-finished extraction.
TEST(SmartExtractTest, AnEncryptedArchiveAsksForAPasswordAndWritesNothingYet) {
    QTemporaryDir work;
    ASSERT_TRUE(work.isValid());
    const QString src = QDir(work.path()).filePath(QStringLiteral("src"));
    ASSERT_FALSE(writeFile(src, QStringLiteral("secret.txt"), "classified").isEmpty());

    const QString zip = QDir(work.path()).filePath(QStringLiteral("locked.zip"));
    QString err;
    if (!ArchiveHandler::create(zip, {QDir(src).filePath(QStringLiteral("secret.txt"))},
                                QStringLiteral("zip"), QStringLiteral("hunter2"), false, 5, &err))
        GTEST_SKIP() << "this build cannot write encrypted zip archives: " << qPrintable(err);

    const QString dest = QDir(work.path()).filePath(QStringLiteral("dest"));
    ASSERT_TRUE(QDir().mkpath(dest));

    const ArchiveHandler::SmartResult noPass = ArchiveHandler::smartExtract(zip, dest, &err);
    EXPECT_FALSE(noPass.ok);
    EXPECT_EQ(noPass.status, ArchiveHandler::Status::NeedPassword);
    EXPECT_TRUE(QDir(dest).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty())
        << "files were written before the password was known";

    // The distinction the recursion depends on: a bad password must be
    // reported as such, not as a generic failure, or the caller stops instead
    // of asking again. buildTree alone does NOT catch it for AES-256 ZIP --
    // its verification read succeeds on the wrong password -- so smartExtract
    // recovers the answer after the extraction fails.
    err.clear();
    const ArchiveHandler::SmartResult wrong =
        ArchiveHandler::smartExtract(zip, dest, QStringLiteral("wrong"), &err);
    EXPECT_FALSE(wrong.ok);
    EXPECT_EQ(wrong.status, ArchiveHandler::Status::WrongPassword)
        << "a wrong password came back as a plain failure, so a caller would give up";

    const ArchiveHandler::SmartResult right =
        ArchiveHandler::smartExtract(zip, dest, QStringLiteral("hunter2"), &err);
    ASSERT_TRUE(right.ok) << qPrintable(err);
    EXPECT_TRUE(QFileInfo::exists(QDir(right.finalDir).filePath(QStringLiteral("secret.txt"))));
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

TEST(SmartExtractTest, SkippingASingleNestedArchiveDoesNotRecurseIntoTheExistingFile) {
    QTemporaryDir work;
    ASSERT_TRUE(work.isValid());
    const QString src = QDir(work.path()).filePath(QStringLiteral("src"));
    const QString incoming = writeFile(src, QStringLiteral("inner.zip"), "incoming archive");
    ASSERT_FALSE(incoming.isEmpty());
    const QString outer = makeZip(work.path(), QStringLiteral("outer"), {incoming});
    if (outer.isEmpty())
        GTEST_SKIP() << "this build cannot write zip archives";

    const QString dest = QDir(work.path()).filePath(QStringLiteral("dest"));
    ASSERT_FALSE(writeFile(dest, QStringLiteral("inner.zip"), "keep existing").isEmpty());
    QString err;
    const ArchiveHandler::SmartResult result = ArchiveHandler::smartExtract(
        outer, dest, QString(), &err, nullptr,
        [](const FileConflict &) { return ErrorAction::Skip; });

    ASSERT_TRUE(result.ok) << qPrintable(err);
    EXPECT_TRUE(result.nestedArchivePath.isEmpty())
        << "a skipped entry made smart extraction recurse into an existing file";
    QFile preserved(QDir(dest).filePath(QStringLiteral("inner.zip")));
    ASSERT_TRUE(preserved.open(QIODevice::ReadOnly));
    EXPECT_EQ(preserved.readAll(), QByteArray("keep existing"));
}
