#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "FolderAssociation.h"

TEST(FolderAssociationTest, ExtractsOnlyExistingDirectoryArguments) {
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString folder = QDir(temp.path()).filePath(QStringLiteral("folder"));
    ASSERT_TRUE(QDir().mkpath(folder));
    const QString file = QDir(temp.path()).filePath(QStringLiteral("file.txt"));
    QFile text(file);
    ASSERT_TRUE(text.open(QIODevice::WriteOnly));

    const QStringList paths = FolderAssociation::folderArguments(
        {QStringLiteral("FileCommander"), folder, file, QStringLiteral("--smoke-test"),
         QDir(temp.path()).filePath(QStringLiteral("missing"))});

    ASSERT_EQ(paths.size(), 1);
    EXPECT_EQ(paths.first(), QDir::cleanPath(QFileInfo(folder).absoluteFilePath()));
}

#ifdef Q_OS_WIN
TEST(FolderAssociationTest, WindowsUsesTheOpenVerbForFolderActivation) {
    EXPECT_EQ(FolderAssociation::windowsOpenVerb(), QStringLiteral("open"));
}
#endif
