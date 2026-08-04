#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QVector>

#include "filesystem/ComputerProvider.h"
#include "filesystem/FileSystemModel.h"

// Renaming a row in the computer view means "relabel this volume", which is a
// much narrower thing than renaming a file. Two properties matter and are
// tested here rather than left to the platform call:
//
//   * The drive LETTER is never in play. It is the volume's identity, and the
//     rename addresses the volume by the catalogued mount root, so nothing the
//     user types can reach it. The editor is seeded with the label alone, which
//     is what makes that true at the UI as well.
//   * Everything else in the listing is read-only. A saved server or a
//     discovered host is named by us or by the network, not by a filesystem.
namespace {

ComputerEntry drive(const QString &label, const QString &root) {
    ComputerEntry entry;
    entry.kind = ComputerEntry::Kind::Drive;
    entry.label = label;
    entry.name = QStringLiteral("%1 (%2)").arg(label, root);
    entry.target = root;
    return entry;
}

ComputerEntry server(const QString &name) {
    ComputerEntry entry;
    entry.kind = ComputerEntry::Kind::SavedServer;
    entry.name = name;
    entry.target = QStringLiteral("uuid-1234");
    return entry;
}

QString pathOfFirstRow(ComputerProvider &provider) {
    const QVector<FileInfo> rows = provider.list(ComputerProvider::rootPath(), true);
    return rows.isEmpty() ? QString() : rows.first().path();
}

} // namespace

TEST(ComputerRename, TheEditorIsSeededWithTheLabelAndNotTheDriveLetter) {
    ComputerProvider provider;
    provider.setEntries({drive(QStringLiteral("ntfs"), QStringLiteral("D:/"))});
    const QString path = pathOfFirstRow(provider);
    ASSERT_FALSE(path.isEmpty());

    EXPECT_EQ(provider.entryRenameSeed(path), QStringLiteral("ntfs"))
        << "seeding with the display name would put \"(D:)\" in the editor, "
           "offering the user a drive letter they cannot change";
}

TEST(ComputerRename, OnlyDrivesAreRenameable) {
    ComputerProvider provider;
    provider.setEntries({server(QStringLiteral("work-nas"))});
    const QString path = pathOfFirstRow(provider);
    ASSERT_FALSE(path.isEmpty());

    EXPECT_FALSE(provider.entryIsRenameable(path));
    QString newPath;
    EXPECT_NE(provider.rename(path, QStringLiteral("anything"), &newPath),
              FileProvider::RenameResult::Ok);
}

TEST(ComputerRename, TheModelRefusesToEditAnythingButTheNameOfARenameableRow) {
    auto provider = std::make_shared<ComputerProvider>();
    provider->setEntries({server(QStringLiteral("work-nas"))});

    FileSystemModel model;
    model.setProvider(provider);
    QSignalSpy loaded(&model, &FileSystemModel::loadFinished);
    model.setRootPath(ComputerProvider::rootPath());
    if (loaded.isEmpty())
        ASSERT_TRUE(loaded.wait(5000));

    for (int column = 0; column < FileSystemModel::ColumnCount; ++column) {
        const QModelIndex index = model.index(0, column);
        ASSERT_TRUE(index.isValid());
        EXPECT_FALSE(model.flags(index) & Qt::ItemIsEditable)
            << "column " << column << " of a non-renameable synthetic row";
    }
}
