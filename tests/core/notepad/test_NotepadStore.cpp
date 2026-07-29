#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

#include "config/Settings.h"
#include "notepad/NotepadStore.h"

namespace {

class IsolatedConfigDir {
public:
    IsolatedConfigDir()
        : wasSet(qEnvironmentVariableIsSet("XDG_CONFIG_HOME")),
          previous(qgetenv("XDG_CONFIG_HOME")) {
        if (dir.isValid())
            qputenv("XDG_CONFIG_HOME", dir.path().toUtf8());
    }

    ~IsolatedConfigDir() {
        if (!dir.isValid())
            return;
        if (wasSet)
            qputenv("XDG_CONFIG_HOME", previous);
        else
            qunsetenv("XDG_CONFIG_HOME");
    }

    bool isValid() const { return dir.isValid(); }
    QTemporaryDir dir;

private:
    bool wasSet;
    QByteArray previous;
};

void writeFile(const QString &path, const QByteArray &data) {
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write(data), data.size());
}

QByteArray readFile(const QString &path) {
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly));
    return file.readAll();
}

} // namespace

TEST(NotepadStoreTest, ExplicitDirectoryPersistsNotes) {
    QTemporaryDir temporaryDir;
    ASSERT_TRUE(temporaryDir.isValid());

    const QString notesPath = temporaryDir.filePath(QStringLiteral("isolated-notes"));
    NotepadStore store(notesPath);
    const NotepadNote note = store.create(QString::fromUtf8("计划"));
    store.save(note.id, QString::fromUtf8("独立内容"), QString());

    NotepadStore reloaded(notesPath);
    ASSERT_EQ(reloaded.notes().size(), 1);
    EXPECT_EQ(reloaded.notes().first().title, QString::fromUtf8("计划"));
    EXPECT_EQ(reloaded.load(note.id), QString::fromUtf8("独立内容"));
}

TEST(NotepadStoreTest, EmptyExplicitDirectoryFallsBackToSafeDefault) {
    IsolatedConfigDir isolated;
    ASSERT_TRUE(isolated.isValid());
    const QString expectedPrefix =
        isolated.dir.filePath(QStringLiteral("FileCommander/notepad/"));
    NotepadStore store{QString()};
    const NotepadNote note = store.create(QStringLiteral("safe-default"));

    EXPECT_TRUE(note.filePath.startsWith(expectedPrefix));
    NotepadStore reloaded;
    ASSERT_EQ(reloaded.notes().size(), 1);
    EXPECT_EQ(reloaded.notes().first().id, note.id);
}

TEST(NotepadStoreTest, ConcurrentStoresDoNotLoseEachOthersNotes) {
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString notesPath = root.filePath(QStringLiteral("notes"));
    NotepadStore firstStore(notesPath);
    NotepadStore secondStore(notesPath);

    const NotepadNote first = firstStore.create(QStringLiteral("first"));
    const NotepadNote second = secondStore.create(QStringLiteral("second"));
    ASSERT_FALSE(first.id.isEmpty());
    ASSERT_FALSE(second.id.isEmpty());

    NotepadStore reloaded(notesPath);
    ASSERT_EQ(reloaded.notes().size(), 2);
    QStringList ids;
    for (const NotepadNote &note : reloaded.notes())
        ids.append(note.id);
    EXPECT_TRUE(ids.contains(first.id));
    EXPECT_TRUE(ids.contains(second.id));
}

TEST(NotepadStoreTest, RejectsStaleConcurrentBodyOverwrite) {
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString notesPath = root.filePath(QStringLiteral("notes"));
    NotepadStore firstStore(notesPath);
    const NotepadNote note = firstStore.create(QStringLiteral("shared"));
    ASSERT_FALSE(note.id.isEmpty());

    NotepadStore secondStore(notesPath);
    const QString firstLoaded = firstStore.load(note.id);
    const QString secondLoaded = secondStore.load(note.id);
    ASSERT_TRUE(firstStore.save(note.id, QStringLiteral("first edit"), firstLoaded));
    EXPECT_FALSE(secondStore.save(note.id, QStringLiteral("stale second edit"), secondLoaded));

    NotepadStore reloaded(notesPath);
    EXPECT_EQ(reloaded.load(note.id), QStringLiteral("first edit"));
}

TEST(NotepadStoreTest, RecoversBodyWhenDeleteStoppedBeforeIndexCommit) {
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString notesPath = root.filePath(QStringLiteral("notes"));
    NotepadStore fixture(notesPath);
    const NotepadNote note = fixture.create(QStringLiteral("recover"));
    ASSERT_FALSE(note.id.isEmpty());
    ASSERT_TRUE(fixture.save(note.id, QStringLiteral("keep me"), QString()));

    const QString tombstone = note.filePath + QStringLiteral(".deleting");
    ASSERT_TRUE(QFile::rename(note.filePath, tombstone));

    NotepadStore recovered(notesPath);
    ASSERT_TRUE(recovered.isAvailable());
    ASSERT_EQ(recovered.notes().size(), 1);
    EXPECT_EQ(recovered.notes().first().id, note.id);
    EXPECT_EQ(recovered.load(note.id), QStringLiteral("keep me"));
    EXPECT_TRUE(QFileInfo::exists(note.filePath));
    EXPECT_FALSE(QFileInfo::exists(tombstone));
}

TEST(NotepadStoreTest, FinishesDeleteWhenIndexCommitPrecededCrash) {
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString notesPath = root.filePath(QStringLiteral("notes"));
    NotepadStore fixture(notesPath);
    const NotepadNote note = fixture.create(QStringLiteral("delete"));
    ASSERT_FALSE(note.id.isEmpty());

    const QString tombstone = note.filePath + QStringLiteral(".deleting");
    ASSERT_TRUE(QFile::rename(note.filePath, tombstone));
    writeFile(notesPath + QStringLiteral("/index.json"), QByteArrayLiteral("[]"));

    NotepadStore recovered(notesPath);
    ASSERT_TRUE(recovered.isAvailable());
    EXPECT_TRUE(recovered.notes().isEmpty());
    EXPECT_FALSE(QFileInfo::exists(note.filePath));
    EXPECT_FALSE(QFileInfo::exists(tombstone));
}

TEST(NotepadStoreTest, RecoversValidOrphanBodyIntoExistingIndex) {
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString notesPath = root.filePath(QStringLiteral("notes"));
    NotepadStore fixture(notesPath);
    const NotepadNote indexed = fixture.create(QStringLiteral("indexed"));
    ASSERT_FALSE(indexed.id.isEmpty());

    const QString orphanId = QStringLiteral("f5ca5942-68ac-4c7d-a235-9c03b21c257e");
    const QString orphanPath = notesPath + QLatin1Char('/') + orphanId + QStringLiteral(".txt");
    writeFile(orphanPath, QByteArrayLiteral("orphan body"));

    NotepadStore recovered(notesPath);
    ASSERT_TRUE(recovered.isAvailable());
    ASSERT_EQ(recovered.notes().size(), 2);
    EXPECT_EQ(recovered.load(orphanId), QStringLiteral("orphan body"));

    NotepadStore reloaded(notesPath);
    ASSERT_EQ(reloaded.notes().size(), 2);
    EXPECT_EQ(reloaded.load(orphanId), QStringLiteral("orphan body"));
}

TEST(NotepadStoreTest, CreatesPrivateDirectoryIndexAndBodyPermissions) {
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString notesPath = root.filePath(QStringLiteral("notes"));
    NotepadStore store(notesPath);
    const NotepadNote note = store.create(QStringLiteral("private"));
    ASSERT_FALSE(note.id.isEmpty());
    ASSERT_TRUE(store.save(note.id, QStringLiteral("secret"), QString()));

    const QFileDevice::Permissions privateDirectory =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
        QFileDevice::ReadUser | QFileDevice::WriteUser | QFileDevice::ExeUser;
    const QFileDevice::Permissions privateFile =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner |
        QFileDevice::ReadUser | QFileDevice::WriteUser;
    EXPECT_EQ(QFileInfo(notesPath).permissions(), privateDirectory);
    EXPECT_EQ(QFileInfo(notesPath + QStringLiteral("/index.json")).permissions(), privateFile);
    EXPECT_EQ(QFileInfo(note.filePath).permissions(), privateFile);
}

TEST(NotepadStoreTest, SaveReportsCommitFailureWhenBodyPathIsDirectory) {
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    NotepadStore store(root.filePath(QStringLiteral("notes")));
    const NotepadNote note = store.create(QStringLiteral("test"));
    ASSERT_FALSE(note.id.isEmpty());
    ASSERT_TRUE(QFile::remove(note.filePath));
    ASSERT_TRUE(QDir().mkpath(note.filePath));

    EXPECT_FALSE(store.save(note.id, QStringLiteral("new body"), QString()));
    EXPECT_TRUE(QFileInfo(note.filePath).isDir());
}

TEST(NotepadStoreTest, RemoveReportsFailureAndKeepsIndexedNote) {
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString notesPath = root.filePath(QStringLiteral("notes"));
    NotepadStore store(notesPath);
    const NotepadNote note = store.create(QStringLiteral("test"));
    ASSERT_FALSE(note.id.isEmpty());
    ASSERT_TRUE(QFile::remove(note.filePath));
    ASSERT_TRUE(QDir().mkpath(note.filePath));

    EXPECT_FALSE(store.remove(note.id));
    ASSERT_EQ(store.notes().size(), 1);
    EXPECT_EQ(store.notes().first().id, note.id);
    EXPECT_TRUE(QString::fromUtf8(readFile(notesPath + QStringLiteral("/index.json")))
                    .contains(note.id));
}

#ifdef Q_OS_UNIX
TEST(NotepadStoreTest, LoadAndSaveRefuseSymbolicLinkBody) {
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString notesPath = root.filePath(QStringLiteral("notes"));
    NotepadStore store(notesPath);
    const NotepadNote note = store.create(QStringLiteral("test"));
    ASSERT_FALSE(note.id.isEmpty());

    const QString victimPath = root.filePath(QStringLiteral("outside.txt"));
    const QByteArray sentinel("outside-secret");
    writeFile(victimPath, sentinel);
    ASSERT_TRUE(QFile::remove(note.filePath));
    ASSERT_EQ(::symlink(victimPath.toLocal8Bit().constData(),
                        note.filePath.toLocal8Bit().constData()),
              0);

    EXPECT_TRUE(store.load(note.id).isEmpty());
    EXPECT_FALSE(store.save(note.id, QStringLiteral("overwrite"), QString()));
    EXPECT_TRUE(QFileInfo(note.filePath).isSymbolicLink());
    EXPECT_EQ(readFile(victimPath), sentinel);
}
#endif

TEST(NotepadStoreTest, UnreadableIndexDoesNotRebuildOrOverwrite) {
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString notesPath = root.filePath(QStringLiteral("notes"));
    ASSERT_TRUE(QDir().mkpath(notesPath));
    writeFile(notesPath + QStringLiteral("/orphan.txt"), QByteArrayLiteral("body"));
    ASSERT_TRUE(QDir().mkpath(notesPath + QStringLiteral("/index.json")));

    NotepadStore store(notesPath);

    EXPECT_FALSE(store.isAvailable());
    EXPECT_TRUE(store.notes().isEmpty());
    EXPECT_TRUE(QFileInfo(notesPath + QStringLiteral("/index.json")).isDir());
}

TEST(NotepadStoreTest, RejectsIndexIdsThatEscapeTheNotesDirectory) {
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString notesPath = root.filePath(QStringLiteral("notes"));
    ASSERT_TRUE(QDir().mkpath(notesPath));
    const QString escapedPath = root.filePath(QStringLiteral("escaped.txt"));
    const QByteArray sentinel("must remain untouched");
    writeFile(escapedPath, sentinel);
    writeFile(notesPath + QStringLiteral("/index.json"),
              QByteArrayLiteral("[{\"id\":\"../escaped\",\"title\":\"malicious\",\"order\":0}]"));

    NotepadStore store(notesPath);
    EXPECT_TRUE(store.notes().isEmpty());
    EXPECT_TRUE(store.load(QStringLiteral("../escaped")).isEmpty());
    store.save(QStringLiteral("../escaped"), QStringLiteral("overwrite attempt"), QString());
    store.remove(QStringLiteral("../escaped"));

    EXPECT_TRUE(QFileInfo::exists(escapedPath));
    EXPECT_EQ(readFile(escapedPath), sentinel);
}
