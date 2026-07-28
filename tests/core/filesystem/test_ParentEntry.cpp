#include <gtest/gtest.h>

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest/QSignalSpy>

#include <memory>

#include "FileInfo.h"
#include "FileProvider.h"
#include "FileSystemModel.h"

// The ".." row.
//
// It was built by running the local-filesystem FileInfo constructor over
// whatever the provider called the parent directory. On a share that path is
// the server's, so ".." showed a same-named LOCAL directory's timestamp and
// permissions -- browsing "/home" on a share gave ".." this machine's "/"
// timestamp -- and where no local namesake existed it degraded to "0 B", type
// "File" and "----------" for a directory that plainly is not that.
namespace {

// Counts what the model asks of it, so "the row is built once per listing"
// can be measured rather than assumed.
class CountingProvider : public FileProvider {
public:
    explicit CountingProvider(bool local) : m_local(local) {}

    bool isLocalFilesystem() const override { return m_local; }
    QString displayName() const override { return m_local ? QString() : QStringLiteral("t@share"); }

    QVector<FileInfo> list(const QString &path, bool) const override {
        QVector<FileInfo> out;
        out.append(FileInfo::fromFields(path + QStringLiteral("/file.txt"),
                                        QStringLiteral("file.txt"), 5,
                                        QDateTime::fromSecsSinceEpoch(1000000), false,
                                        QFile::ReadOwner));
        return out;
    }
    bool isDir(const QString &) const override { return true; }
    QString cleanPath(const QString &p) const override { return p; }
    QString parentPath(const QString &path) const override {
        ++parentPathCalls;
        const int slash = path.lastIndexOf(QLatin1Char('/'));
        if (slash < 0 || path == QLatin1String("/"))
            return {}; // the backend's root has no parent
        return slash == 0 ? QStringLiteral("/") : path.left(slash);
    }
    bool exists(const QString &) const override { return true; }
    RenameResult rename(const QString &, const QString &, QString *) override {
        return RenameResult::Failed;
    }

    mutable int parentPathCalls = 0;

private:
    bool m_local;
};

bool loadDir(FileSystemModel &model, const QString &path) {
    QSignalSpy spy(&model, &FileSystemModel::loadFinished);
    model.setRootPath(path);
    return spy.count() > 0 || spy.wait(4000);
}

QString displayAt(const FileSystemModel &model, int row, int column) {
    return model.data(model.index(row, column), Qt::DisplayRole).toString();
}

} // namespace

// --- the row itself ---------------------------------------------------------

TEST(ParentEntryTest, LocalParentIsStillReadFromDisk) {
    // Unchanged behaviour on a local tab: ".." carries the real parent's
    // timestamp and permissions, which is what makes it sortable and useful.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString child = QDir(dir.path()).filePath(QStringLiteral("sub"));
    ASSERT_TRUE(QDir().mkpath(child));

    const FileInfo info = FileInfo::makeParentEntry(dir.path(), /*localFilesystem=*/true);
    EXPECT_EQ(info.name().toStdString(), "..");
    EXPECT_EQ(info.path(), dir.path());
    EXPECT_TRUE(info.isDir());
    EXPECT_TRUE(info.isParentEntry());
    EXPECT_TRUE(info.modified().isValid());
    EXPECT_EQ(info.modified(), QFileInfo(dir.path()).lastModified());
    EXPECT_TRUE(info.hasPermissions());
    EXPECT_EQ(info.permissionsString().at(0), QLatin1Char('d'));
}

TEST(ParentEntryTest, RemoteParentIsNotStatedLocally) {
    // "/home" exists on this machine too. The row must carry nothing borrowed
    // from it -- no timestamp, no permissions, no size.
    const FileInfo info = FileInfo::makeParentEntry(QStringLiteral("/home"),
                                                    /*localFilesystem=*/false);
    EXPECT_EQ(info.name().toStdString(), "..");
    EXPECT_EQ(info.path().toStdString(), "/home");
    EXPECT_TRUE(info.isParentEntry());
    // It is a directory -- that much is known without asking anyone.
    EXPECT_TRUE(info.isDir());
    // And nothing else is.
    EXPECT_FALSE(info.modified().isValid())
        << "took a timestamp from this machine's /home";
    const QDateTime localModified = QFileInfo(QStringLiteral("/home")).lastModified();
    if (localModified.isValid())
        EXPECT_NE(info.modified(), localModified);
    EXPECT_EQ(info.size(), 0);
    EXPECT_FALSE(info.hasPermissions());
    EXPECT_TRUE(info.permissionsString().isEmpty())
        << "claimed " << info.permissionsString().toStdString()
        << " about a directory on a server";
}

TEST(ParentEntryTest, RemoteParentWithNoLocalNamesakeIsStillADirectory) {
    // The other half of the old failure: with no local namesake the row fell
    // back to a zero-length *file*, so ".." sorted among the files and drew a
    // file icon.
    const FileInfo info = FileInfo::makeParentEntry(
        QStringLiteral("/no/such/place/on/this/machine"), /*localFilesystem=*/false);
    EXPECT_TRUE(info.isDir());
    EXPECT_TRUE(info.isValid());
}

// --- the row as the model renders it ----------------------------------------

TEST(ParentEntryTest, ModelRendersRemoteParentWithEmptyColumnsNotBorrowedOnes) {
    auto provider = std::make_shared<CountingProvider>(/*local=*/false);
    FileSystemModel model;
    model.setProvider(provider);
    ASSERT_TRUE(loadDir(model, QStringLiteral("/home")));
    ASSERT_TRUE(model.isParentEntry(0));

    EXPECT_EQ(displayAt(model, 0, FileSystemModel::NameColumn).toStdString(), "..");
    EXPECT_EQ(displayAt(model, 0, FileSystemModel::SizeColumn).toStdString(), "<DIR>");
    EXPECT_TRUE(displayAt(model, 0, FileSystemModel::ModifiedColumn).isEmpty())
        << "showed " << displayAt(model, 0, FileSystemModel::ModifiedColumn).toStdString()
        << ", which can only have come from this machine";
    EXPECT_TRUE(displayAt(model, 0, FileSystemModel::CreatedColumn).isEmpty());
    EXPECT_TRUE(displayAt(model, 0, FileSystemModel::PermissionsColumn).isEmpty());
    EXPECT_TRUE(model.data(model.index(0, 0), FileSystemModel::IsDirRole).toBool());
}

TEST(ParentEntryTest, ModelStillRendersLocalParentFully) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString child = QDir(dir.path()).filePath(QStringLiteral("sub"));
    ASSERT_TRUE(QDir().mkpath(child));

    FileSystemModel model; // defaults to the local backend
    ASSERT_TRUE(loadDir(model, child));
    ASSERT_TRUE(model.isParentEntry(0));

    EXPECT_EQ(displayAt(model, 0, FileSystemModel::NameColumn).toStdString(), "..");
    EXPECT_EQ(displayAt(model, 0, FileSystemModel::SizeColumn).toStdString(), "<DIR>");
    EXPECT_FALSE(displayAt(model, 0, FileSystemModel::ModifiedColumn).isEmpty())
        << "a local '..' must keep showing the parent's date";
    EXPECT_EQ(displayAt(model, 0, FileSystemModel::PermissionsColumn).at(0), QLatin1Char('d'));
    EXPECT_EQ(model.fileInfoAt(0).path(), dir.path());
}

TEST(ParentEntryTest, TheRowIsBuiltOncePerListingNotPerQuery) {
    // Every query that touched row 0 used to rebuild the row -- and on a local
    // backend building it means a stat(). A repaint asks for several roles
    // across seven columns, so a directory listing ran a stream of stat()s on
    // the GUI thread; on a hung autofs/NFS mount that is a frozen window.
    auto provider = std::make_shared<CountingProvider>(/*local=*/false);
    FileSystemModel model;
    model.setProvider(provider);
    ASSERT_TRUE(loadDir(model, QStringLiteral("/share/docs")));
    ASSERT_TRUE(model.isParentEntry(0));

    provider->parentPathCalls = 0;
    for (int repaint = 0; repaint < 20; ++repaint) {
        for (int col = 0; col < FileSystemModel::ColumnCount; ++col) {
            const QModelIndex idx = model.index(0, col);
            model.data(idx, Qt::DisplayRole);
            model.data(idx, Qt::DecorationRole);
            model.data(idx, Qt::TextAlignmentRole);
        }
        model.fileInfoAt(0);
    }
    EXPECT_EQ(provider->parentPathCalls, 0)
        << "rebuilt the '..' row " << provider->parentPathCalls << " times while painting";
}

TEST(ParentEntryTest, NoParentRowAtARoot) {
    auto provider = std::make_shared<CountingProvider>(/*local=*/false);
    FileSystemModel model;
    model.setProvider(provider);
    ASSERT_TRUE(loadDir(model, QStringLiteral("/")));
    EXPECT_FALSE(model.isParentEntry(0)) << "offered a way up out of the server's root";
    EXPECT_EQ(model.fileInfoAt(0).name().toStdString(), "file.txt");
}
