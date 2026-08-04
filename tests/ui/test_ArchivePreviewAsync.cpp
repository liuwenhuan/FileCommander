#include <gtest/gtest.h>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QProcess>
#include <QTest>
#include <QThread>
#include <QVector>
#include <QtConcurrent/QtConcurrent>

#include "FileListView.h"
#include "FileSystemModel.h"

#include "ArchiveProvider.h"
#include "FilePanel.h"

// Previewing a picture inside a big archive froze the window. A non-zip archive
// is extracted WHOLE the first time any entry is touched, and that extraction
// ran on the GUI thread inside currentPreviewPath() -- so opening one image out
// of a few hundred unpacked all of them with nothing on screen to say so.
//
// The split is: ask what is ready (never blocks), and start the rest on a
// worker.
namespace {

// A .tar of `count` small PNGs. Tar rather than zip on purpose: zip extracts
// one entry at a time, tar is the extract-everything case this is about.
QString writeTarOfImages(const QDir &root, int count) {
    QDir(root).mkpath(QStringLiteral("stage"));
    const QDir stage(root.filePath(QStringLiteral("stage")));
    for (int i = 0; i < count; ++i) {
        QImage image(64, 64, QImage::Format_ARGB32);
        image.fill(Qt::magenta);
        image.save(stage.filePath(QStringLiteral("shot%1.png").arg(i, 3, 10, QLatin1Char('0'))));
    }

    const QString archive = root.filePath(QStringLiteral("album.tar"));
    QProcess tar;
    tar.setWorkingDirectory(stage.path());
    tar.start(QStringLiteral("tar"), {QStringLiteral("-cf"), archive, QStringLiteral(".")});
    if (!tar.waitForFinished(30000) || tar.exitCode() != 0)
        return {};
    return QFile::exists(archive) ? archive : QString();
}

} // namespace

TEST(ArchivePreviewAsync, AskingWhatIsReadyNeverExtracts) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QDir root(dir.path());
    const QString archive = writeTarOfImages(root, 40);
    if (archive.isEmpty())
        GTEST_SKIP() << "no tar available to build the fixture";

    QString error;
    auto provider = std::make_shared<ArchiveProvider>(archive, &error);
    ASSERT_TRUE(error.isEmpty()) << error.toStdString();

    const QVector<FileInfo> entries = provider->list(QStringLiteral("/"), true);
    QString firstFile;
    for (const FileInfo &entry : entries) {
        if (!entry.isDir()) {
            firstFile = entry.path();
            break;
        }
    }
    ASSERT_FALSE(firstFile.isEmpty()) << "fixture has no files";

    // Nothing extracted yet, so there is nothing ready -- and asking must not
    // be what triggers the extraction.
    QElapsedTimer timer;
    timer.start();
    EXPECT_TRUE(provider->materializedPathIfReady(firstFile).isEmpty());
    EXPECT_LT(timer.elapsed(), 200)
        << "asking what is ready took " << timer.elapsed() << "ms -- it extracted";

    // ...and once it has been extracted, the same question answers without
    // doing the work again.
    const QString real = provider->materialize(firstFile);
    ASSERT_FALSE(real.isEmpty());
    timer.restart();
    EXPECT_EQ(provider->materializedPathIfReady(firstFile), real);
    EXPECT_LT(timer.elapsed(), 200);
}

TEST(ArchivePreviewAsync, ThePanelExtractsOffTheGuiThreadAndReportsBack) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QDir root(dir.path());
    const QString archive = writeTarOfImages(root, 40);
    if (archive.isEmpty())
        GTEST_SKIP() << "no tar available to build the fixture";

    FilePanel panel;
    panel.resize(700, 500);
    panel.show();
    QTest::qWaitForWindowExposed(&panel);
    panel.navigateTo(root.path());
    QTest::qWait(300);

    ASSERT_TRUE(panel.enterArchive(archive, archive, false)) << "could not enter the archive";
    QTest::qWait(300);

    // Put the cursor on a real entry.
    FileSystemModel *model = panel.model();
    int row = -1;
    for (int r = 0; r < model->rowCount(); ++r) {
        if (!model->isParentEntry(r) && !model->fileInfoAt(r).isDir()) {
            row = r;
            break;
        }
    }
    ASSERT_GE(row, 0) << "no file rows inside the archive";
    panel.view()->setCurrentIndex(model->index(row, 0));

    // Nothing is extracted yet, so the panel must say "not ready" rather than
    // block -- this is the call the GUI thread makes.
    QElapsedTimer timer;
    timer.start();
    EXPECT_TRUE(panel.currentPreviewPathIfReady().isEmpty());
    EXPECT_LT(timer.elapsed(), 200)
        << "the ready check blocked for " << timer.elapsed() << "ms";

    QSignalSpy extracted(&panel, &FilePanel::previewExtracted);
    panel.beginPreviewExtraction();
    // Returning promptly is the whole point: the extraction is on a worker.
    EXPECT_LT(timer.elapsed(), 500);

    ASSERT_TRUE(extracted.wait(20000)) << "the extraction never reported back";
    const QList<QVariant> args = extracted.takeFirst();
    EXPECT_EQ(args.at(0).toString(), model->fileInfoAt(row).path());
    const QString local = args.at(1).toString();
    ASSERT_FALSE(local.isEmpty()) << "extraction produced nothing";
    EXPECT_TRUE(QFile::exists(local)) << local.toStdString();

    // ...and now the cheap question answers.
    EXPECT_EQ(panel.currentPreviewPathIfReady(), local);
}

// The one that matters, and the one the first version of this file missed: the
// ready check has to answer WHILE an extraction is running. Asking when nothing
// is in flight proves nothing -- the first attempt at this fix had the check
// take the same mutex materialize() holds for the whole extraction, so every
// cursor move still froze the window for the length of the unpack.
TEST(ArchivePreviewAsync, TheReadyCheckAnswersWhileAnExtractionIsRunning) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QDir root(dir.path());
    // Enough images that the extraction lasts long enough to ask during it.
    const QString archive = writeTarOfImages(root, 400);
    if (archive.isEmpty())
        GTEST_SKIP() << "no tar available to build the fixture";

    QString error;
    auto provider = std::make_shared<ArchiveProvider>(archive, &error);
    ASSERT_TRUE(error.isEmpty()) << error.toStdString();

    QString first;
    QString second;
    for (const FileInfo &entry : provider->list(QStringLiteral("/"), true)) {
        if (entry.isDir())
            continue;
        if (first.isEmpty())
            first = entry.path();
        else if (second.isEmpty())
            second = entry.path();
    }
    ASSERT_FALSE(first.isEmpty());
    ASSERT_FALSE(second.isEmpty());

    // Start a real extraction on another thread and keep asking while it runs.
    QFuture<QString> running =
        QtConcurrent::run([provider, first]() { return provider->materialize(first); });

    qint64 worst = 0;
    QElapsedTimer overall;
    overall.start();
    int asks = 0;
    while (!running.isFinished() && overall.elapsed() < 20000) {
        QElapsedTimer ask;
        ask.start();
        provider->materializedPathIfReady(second);
        worst = qMax(worst, ask.elapsed());
        ++asks;
        QThread::msleep(5);
    }
    running.waitForFinished();
    ASSERT_FALSE(running.result().isEmpty()) << "the extraction itself failed";
    ASSERT_GT(asks, 0) << "the extraction finished too fast to ask during it";

    // This is what the GUI thread does on every cursor move. Stated as a
    // FRACTION of the extraction, so the assertion holds whatever the machine
    // and fixture size make of it: taking the extraction's own mutex means
    // blocking for most of what is left of it (measured 83ms of a 111ms
    // unpack), while not taking it means microseconds.
    const qint64 extraction = overall.elapsed();
    EXPECT_LT(worst, qMax<qint64>(30, extraction / 4))
        << "the ready check blocked " << worst << "ms of a " << extraction
        << "ms extraction -- that is the freeze";
}
