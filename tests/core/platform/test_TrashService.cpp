#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "TrashService.h"

TEST(TrashServiceTest, WindowsTrashEntryCanBeRestoredByItsUndoToken) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("restore-me.txt"));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("restore me");
    file.close();

    std::unique_ptr<TrashService> trash = createTrashService();
    ASSERT_NE(trash, nullptr);
    const PlatformResult removed = trash->moveToTrash({path});
    ASSERT_TRUE(removed.ok) << removed.message.toStdString();
    ASSERT_FALSE(QFile::exists(path));
    ASSERT_EQ(removed.undoEntries.size(), 1)
        << "a successful single-item trash operation must identify its new Recycle Bin item";

    const PlatformResult restored = trash->restoreFromTrash(removed.undoEntries);
    ASSERT_TRUE(restored.ok) << restored.message.toStdString();
    ASSERT_TRUE(QFile::exists(path));
    QFile restoredFile(path);
    ASSERT_TRUE(restoredFile.open(QIODevice::ReadOnly));
    EXPECT_EQ(restoredFile.readAll(), QByteArray("restore me"));
}
