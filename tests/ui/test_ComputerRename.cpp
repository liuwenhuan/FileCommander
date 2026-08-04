#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QVector>

#include "filesystem/ComputerProvider.h"
#include "filesystem/FileSystemModel.h"

// Nothing in the computer view can be renamed. Every row names a *place* -- a
// drive, a saved bookmark, a host that answered a scan -- and none of those
// names is a file manager's to change. Renaming a drive would have meant
// relabelling the volume, which is a different operation wearing a rename's
// clothes; the drive letter it appears to offer is not editable at all.
//
// Held down by a test because the read-only-ness is spread across three places
// (flags withholds the editor, setData refuses a direct call, the provider
// refuses the rename itself) and any one of them silently regressing would put
// an editor back on a row that cannot honour it.
namespace {

ComputerEntry drive(const QString &label, const QString &root) {
    ComputerEntry entry;
    entry.kind = ComputerEntry::Kind::Drive;
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

} // namespace

TEST(ComputerRename, NoRowInTheViewOffersAnEditor) {
    auto provider = std::make_shared<ComputerProvider>();
    provider->setEntries({drive(QStringLiteral("ntfs"), QStringLiteral("D:/")),
                          server(QStringLiteral("work-nas"))});

    FileSystemModel model;
    model.setProvider(provider);
    QSignalSpy loaded(&model, &FileSystemModel::loadFinished);
    model.setRootPath(ComputerProvider::rootPath());
    if (loaded.isEmpty())
        ASSERT_TRUE(loaded.wait(5000));
    ASSERT_GT(model.rowCount(), 0);

    for (int row = 0; row < model.rowCount(); ++row) {
        for (int column = 0; column < FileSystemModel::ColumnCount; ++column) {
            const QModelIndex index = model.index(row, column);
            ASSERT_TRUE(index.isValid());
            EXPECT_FALSE(model.flags(index) & Qt::ItemIsEditable)
                << "row " << row << " column " << column;
        }
    }
}

TEST(ComputerRename, TheModelRefusesAnEditCommittedDirectly) {
    auto provider = std::make_shared<ComputerProvider>();
    provider->setEntries({drive(QStringLiteral("ntfs"), QStringLiteral("D:/"))});

    FileSystemModel model;
    model.setProvider(provider);
    QSignalSpy loaded(&model, &FileSystemModel::loadFinished);
    model.setRootPath(ComputerProvider::rootPath());
    if (loaded.isEmpty())
        ASSERT_TRUE(loaded.wait(5000));
    ASSERT_GT(model.rowCount(), 0);

    // flags() already withholds the editor; setData is public, so it has to
    // refuse on its own rather than trust that nobody reaches it.
    EXPECT_FALSE(model.setData(model.index(0, FileSystemModel::NameColumn),
                               QStringLiteral("renamed"), Qt::EditRole));
}

TEST(ComputerRename, TheProviderRefusesEveryRename) {
    ComputerProvider provider;
    provider.setEntries({drive(QStringLiteral("ntfs"), QStringLiteral("D:/")),
                         server(QStringLiteral("work-nas"))});

    for (const FileInfo &row : provider.list(ComputerProvider::rootPath(), true)) {
        QString newPath;
        EXPECT_NE(provider.rename(row.path(), QStringLiteral("anything"), &newPath),
                  FileProvider::RenameResult::Ok)
            << row.name().toStdString();
    }
}
