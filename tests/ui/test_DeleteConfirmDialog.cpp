#include <gtest/gtest.h>

#include <QApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>

#include "FileSystemModel.h"
#include "dialogs/DeleteConfirmDialog.h"

// The delete confirmation has one job beyond asking the question: say how much
// is about to be destroyed. It used to add up only the selected FILES, because
// a directory's listed size is its own entry, not its contents -- so selecting
// folders offered gigabytes for deletion as "0 bytes", and a mixed selection
// looked like the folders had simply not been counted.
namespace {

QString write(const QString &path, int bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    file.write(QByteArray(bytes, 'x'));
    file.close();
    return path;
}

// Loads `dir` into a model the way a panel does, so the summary is read out of
// a real listing rather than a hand-made one.
void loadInto(FileSystemModel &model, const QString &dir) {
    QSignalSpy loaded(&model, &FileSystemModel::loadFinished);
    model.setRootPath(dir);
    if (loaded.isEmpty())
        loaded.wait(5000);
    qApp->processEvents();
}

QString pathOf(const FileSystemModel &model, const QString &name) {
    for (int row = 0; row < model.rowCount(); ++row) {
        const FileInfo info = model.fileInfoAt(row);
        if (info.name() == name)
            return info.path();
    }
    return {};
}

// Runs the dialog's background measurement to completion without showing it.
void settleMeasurement(DeleteConfirmDialog &dialog) {
    if (!dialog.isMeasuring())
        return;
    QEventLoop loop;
    QObject::connect(&dialog, &DeleteConfirmDialog::measurementFinished, &loop,
                     &QEventLoop::quit);
    QTimer::singleShot(15000, &loop, &QEventLoop::quit);
    loop.exec();
}

QLabel *labelNamed(DeleteConfirmDialog &dialog, const QString &name) {
    return dialog.findChild<QLabel *>(name);
}

} // namespace

TEST(DeleteSelectionSummaryTest, CountsFilesAndFoldersSeparately) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_FALSE(write(QDir(dir.path()).filePath(QStringLiteral("a.bin")), 500).isEmpty());
    ASSERT_FALSE(write(QDir(dir.path()).filePath(QStringLiteral("b.bin")), 300).isEmpty());
    ASSERT_FALSE(
        write(QDir(dir.path()).filePath(QStringLiteral("tree/inner.bin")), 1000).isEmpty());

    FileSystemModel model;
    loadInto(model, dir.path());

    const QStringList selection{pathOf(model, QStringLiteral("a.bin")),
                                pathOf(model, QStringLiteral("b.bin")),
                                pathOf(model, QStringLiteral("tree"))};
    for (const QString &path : selection)
        ASSERT_FALSE(path.isEmpty());

    const DeleteSelectionSummary summary = summarizeDeleteSelection(&model, selection);
    EXPECT_EQ(summary.fileCount, 2);
    EXPECT_EQ(summary.folderCount, 1);
    // The listing cannot know what is inside the folder, and must not pretend
    // the folder's own entry size is it.
    EXPECT_EQ(summary.listedBytes, 800);
}

// The reported bug: the number in front of the user has to cover the folders.
TEST(DeleteConfirmDialogTest, SizeIncludesTheContentsOfSelectedFolders) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    write(QDir(dir.path()).filePath(QStringLiteral("loose.bin")), 500);
    write(QDir(dir.path()).filePath(QStringLiteral("tree/one.bin")), 1000);
    write(QDir(dir.path()).filePath(QStringLiteral("tree/nested/two.bin")), 2000);

    FileSystemModel model;
    loadInto(model, dir.path());
    const QStringList selection{pathOf(model, QStringLiteral("loose.bin")),
                                pathOf(model, QStringLiteral("tree"))};

    DeleteConfirmDialog dialog(selection, summarizeDeleteSelection(&model, selection),
                               /*permanent=*/false, /*measureLocally=*/true);
    settleMeasurement(dialog);

    // 500 loose + 1000 + 2000 nested.
    EXPECT_TRUE(dialog.sizeText().contains(QStringLiteral("3,500"))
                || dialog.sizeText().contains(QStringLiteral("3500")))
        << dialog.sizeText().toStdString();
    EXPECT_FALSE(dialog.sizeText().contains(QStringLiteral("500 B")))
        << "still showing only the loose file: " << dialog.sizeText().toStdString();
}

TEST(DeleteConfirmDialogTest, FolderOnlySelectionIsNotOfferedAsZeroBytes) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    write(QDir(dir.path()).filePath(QStringLiteral("big/a.bin")), 4096);
    write(QDir(dir.path()).filePath(QStringLiteral("big/b.bin")), 4096);

    FileSystemModel model;
    loadInto(model, dir.path());
    const QStringList selection{pathOf(model, QStringLiteral("big"))};

    DeleteConfirmDialog dialog(selection, summarizeDeleteSelection(&model, selection), false,
                               true);
    settleMeasurement(dialog);

    EXPECT_TRUE(dialog.sizeText().contains(QStringLiteral("8,192"))
                || dialog.sizeText().contains(QStringLiteral("8192")))
        << dialog.sizeText().toStdString();
}

// The user must be able to answer immediately; the figure catches up.
TEST(DeleteConfirmDialogTest, IsAnswerableBeforeTheMeasurementFinishes) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    write(QDir(dir.path()).filePath(QStringLiteral("tree/one.bin")), 1024);

    FileSystemModel model;
    loadInto(model, dir.path());
    const QStringList selection{pathOf(model, QStringLiteral("tree"))};

    DeleteConfirmDialog dialog(selection, summarizeDeleteSelection(&model, selection), false,
                               true);

    auto *buttons = dialog.findChild<QDialogButtonBox *>();
    ASSERT_NE(buttons, nullptr);
    EXPECT_TRUE(buttons->button(QDialogButtonBox::Yes)->isEnabled());
    EXPECT_TRUE(buttons->button(QDialogButtonBox::No)->isEnabled());
    // Deleting is the destructive answer, so it must not be the one a stray
    // Return key picks.
    EXPECT_TRUE(buttons->button(QDialogButtonBox::No)->isDefault());
    EXPECT_FALSE(buttons->button(QDialogButtonBox::Yes)->isDefault());

    settleMeasurement(dialog);
}

TEST(DeleteConfirmDialogTest, SaysHowManyFilesAndHowManyFolders) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    write(QDir(dir.path()).filePath(QStringLiteral("a.bin")), 10);
    write(QDir(dir.path()).filePath(QStringLiteral("b.bin")), 10);
    write(QDir(dir.path()).filePath(QStringLiteral("t1/x.bin")), 10);
    write(QDir(dir.path()).filePath(QStringLiteral("t2/x.bin")), 10);

    FileSystemModel model;
    loadInto(model, dir.path());
    const QStringList selection{
        pathOf(model, QStringLiteral("a.bin")), pathOf(model, QStringLiteral("b.bin")),
        pathOf(model, QStringLiteral("t1")), pathOf(model, QStringLiteral("t2"))};

    DeleteConfirmDialog dialog(selection, summarizeDeleteSelection(&model, selection), false,
                               true);
    settleMeasurement(dialog);

    const QString text = dialog.summaryText();
    EXPECT_TRUE(text.contains(QStringLiteral("2 file"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("2 folder"))) << text.toStdString();
}

// A network tab's paths belong to the server. Walking them would mean thousands
// of round trips to answer a question that is about to be dismissed, so the
// dialog reports what the listing knows -- and says that is what it is doing,
// rather than presenting a partial figure as the total.
TEST(DeleteConfirmDialogTest, ARemoteSelectionIsNotWalkedAndSaysSo) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    write(QDir(dir.path()).filePath(QStringLiteral("a.bin")), 700);
    write(QDir(dir.path()).filePath(QStringLiteral("tree/inner.bin")), 5000);

    FileSystemModel model;
    loadInto(model, dir.path());
    const QStringList selection{pathOf(model, QStringLiteral("a.bin")),
                                pathOf(model, QStringLiteral("tree"))};

    DeleteConfirmDialog dialog(selection, summarizeDeleteSelection(&model, selection),
                               /*permanent=*/true, /*measureLocally=*/false);

    EXPECT_FALSE(dialog.isMeasuring());
    EXPECT_TRUE(dialog.sizeText().contains(QStringLiteral("700")))
        << dialog.sizeText().toStdString();
    EXPECT_TRUE(dialog.sizeText().contains(QStringLiteral("folder"), Qt::CaseInsensitive))
        << "nothing tells the user the folders are missing from this figure: "
        << dialog.sizeText().toStdString();
}

TEST(DeleteConfirmDialogTest, APermanentDeleteSaysItSkipsTheTrash) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    write(QDir(dir.path()).filePath(QStringLiteral("a.bin")), 10);

    FileSystemModel model;
    loadInto(model, dir.path());
    const QStringList selection{pathOf(model, QStringLiteral("a.bin"))};
    const DeleteSelectionSummary summary = summarizeDeleteSelection(&model, selection);

    DeleteConfirmDialog permanent(selection, summary, /*permanent=*/true, true);
    ASSERT_NE(labelNamed(permanent, QStringLiteral("DeleteConfirmWarning")), nullptr);
    EXPECT_TRUE(labelNamed(permanent, QStringLiteral("DeleteConfirmWarning"))
                    ->text()
                    .contains(QStringLiteral("trash"), Qt::CaseInsensitive));

    DeleteConfirmDialog toTrash(selection, summary, /*permanent=*/false, true);
    EXPECT_EQ(labelNamed(toTrash, QStringLiteral("DeleteConfirmWarning")), nullptr)
        << "a trash delete should not warn about being permanent";
}
