#include <gtest/gtest.h>

#include <QTemporaryDir>

#include "notepad/NotepadStore.h"

TEST(NotepadStoreTest, ExplicitDirectoryPersistsNotes) {
    QTemporaryDir temporaryDir;
    ASSERT_TRUE(temporaryDir.isValid());

    const QString notesPath = temporaryDir.filePath(QStringLiteral("isolated-notes"));
    NotepadStore store(notesPath);
    const NotepadNote note = store.create(QString::fromUtf8("计划"));
    store.save(note.id, QString::fromUtf8("独立内容"));

    NotepadStore reloaded(notesPath);
    ASSERT_EQ(reloaded.notes().size(), 1);
    EXPECT_EQ(reloaded.notes().first().title, QString::fromUtf8("计划"));
    EXPECT_EQ(reloaded.load(note.id), QString::fromUtf8("独立内容"));
}
