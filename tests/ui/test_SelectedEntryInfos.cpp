#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QItemSelectionModel>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QVector>

#include <memory>

#include "FilePanel.h"
#include "FileProvider.h"
#include "IconFileView.h"
#include "FileSystemModel.h"
#include "dialogs/PropertiesDialog.h"

// FilePanel::selectedEntryInfos() -- the cached listing behind the selection.
//
// Everything that used to stat a network tab's selection locally (the Properties
// summary, the checksum filter, the compare filter) now reads it from here
// instead, so what matters is that the sizes and the isDir flags survive the
// trip: a local QFileInfo over those same paths reports nothing at all.
namespace {

// A stand-in server: one directory of fixed entries, immutable once built, which
// is what makes it safe to hand to NetworkSession's worker thread.
class FakeShare : public FileProvider {
public:
    explicit FakeShare(QString dir) : m_dir(std::move(dir)) {}

    void addFile(const QString &name, qint64 size) { m_entries.append({name, size, false}); }
    void addDir(const QString &name) { m_entries.append({name, 4096, true}); }

    QString displayName() const override { return QStringLiteral("tester@share"); }
    QString scheme() const override { return QStringLiteral("smb"); }

    QVector<FileInfo> list(const QString &path, bool) const override {
        QVector<FileInfo> out;
        if (cleanPath(path) != m_dir)
            return out;
        for (const Entry &e : m_entries)
            out.append(FileInfo::fromFields(m_dir + QLatin1Char('/') + e.name, e.name, e.size,
                                            QDateTime::currentDateTime(), e.isDir,
                                            PropertiesDialog::fromOctal(0744)));
        return out;
    }
    bool isDir(const QString &path) const override { return cleanPath(path) == m_dir; }
    QString cleanPath(const QString &path) const override {
        QString p = path;
        while (p.size() > 1 && p.endsWith(QLatin1Char('/')))
            p.chop(1);
        return p;
    }
    QString parentPath(const QString &path) const override {
        const QString clean = cleanPath(path);
        const int slash = clean.lastIndexOf(QLatin1Char('/'));
        if (slash < 0)
            return {};
        return slash == 0 ? QStringLiteral("/") : clean.left(slash);
    }
    bool exists(const QString &) const override { return true; }
    RenameResult rename(const QString &, const QString &, QString *) override {
        return RenameResult::Failed;
    }

private:
    struct Entry {
        QString name;
        qint64 size;
        bool isDir;
    };
    QString m_dir;
    QVector<Entry> m_entries;
};

void settle(FilePanel &panel) {
    QSignalSpy spy(panel.model(), &FileSystemModel::loadFinished);
    if (spy.isEmpty())
        spy.wait(4000);
    QCoreApplication::processEvents();
}

void navigate(FilePanel &panel, const QString &path) {
    QSignalSpy spy(panel.model(), &FileSystemModel::loadFinished);
    panel.navigateTo(path);
    if (spy.isEmpty())
        spy.wait(4000);
    QCoreApplication::processEvents();
}

} // namespace

TEST(SelectedEntryInfosTest, NetworkSelectionCarriesSizesAndDirectoryFlags) {
    const QString remoteDir = QStringLiteral("/share/docs");
    auto share = std::make_shared<FakeShare>(remoteDir);
    share->addFile(QStringLiteral("a.pdf"), 3 * 1024 * 1024);
    share->addFile(QStringLiteral("b.iso"), 5 * 1024 * 1024);
    share->addDir(QStringLiteral("sub"));

    FilePanel panel;
    panel.connectTabTo(0, share, [](QString *) { return true; }, remoteDir,
                       QStringLiteral("tester@share"), SavedConnection{},
                       FileSystemModel::AuthRetryFactory());
    settle(panel);
    ASSERT_TRUE(panel.model()->hasNetworkSession());

    panel.selectAll();
    const QVector<FileInfo> infos = panel.selectedEntryInfos();
    ASSERT_EQ(infos.size(), 3);

    // Exactly what the Properties summary now reports -- and the number the old
    // local-stat path could never produce for a server's paths.
    EXPECT_EQ(PropertiesDialog::totalFileSize(infos), 8LL * 1024 * 1024);

    int dirs = 0;
    for (const FileInfo &info : infos) {
        EXPECT_FALSE(QFileInfo(info.path()).exists())
            << "the test's premise: these paths mean nothing to the local filesystem";
        EXPECT_EQ(PropertiesDialog::toOctal(info.permissions()), 0744);
        if (info.isDir())
            ++dirs;
    }
    EXPECT_EQ(dirs, 1) << "the directory must stay distinguishable, so checksums can skip it";
}

TEST(SelectedEntryInfosTest, LocalSelectionMatchesSelectedPaths) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    for (const char *name : {"one.txt", "two.txt"}) {
        QFile f(QDir(dir.path()).filePath(QString::fromLatin1(name)));
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("12345");
        f.close();
    }

    FilePanel panel;
    navigate(panel, dir.path());
    panel.selectAll();

    const QStringList paths = panel.selectedPaths();
    const QVector<FileInfo> infos = panel.selectedEntryInfos();
    ASSERT_EQ(infos.size(), paths.size());
    for (int i = 0; i < infos.size(); ++i)
        EXPECT_EQ(infos.at(i).path(), paths.at(i));
    EXPECT_EQ(PropertiesDialog::totalFileSize(infos), 10);
}

// Thumbnail mode marks the shared selection model the way QListView/IconMode
// does -- single items in column 0, not whole rows. selectedRows() reports
// nothing for that, so selectedPaths() used to see an empty selection and fall
// back to the cursor row: a Ctrl-selected copy of three files delivered one.
TEST(SelectedEntryInfosTest, ThumbnailModeSelectionIsNotCollapsedToOneEntry) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QStringList names{QStringLiteral("f1.txt"), QStringLiteral("f2.txt"),
                            QStringLiteral("f3.txt"), QStringLiteral("f4.txt"),
                            QStringLiteral("f5.txt")};
    for (const QString &name : names) {
        QFile f(QDir(dir.path()).filePath(name));
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("abc");
        f.close();
    }

    FilePanel panel;
    navigate(panel, dir.path());
    panel.toggleViewMode();
    ASSERT_TRUE(panel.isThumbnailMode());

    // Ctrl-click f1, f3, f5: the icon view selects the item, never the row.
    QItemSelectionModel *sel = panel.iconView()->selectionModel();
    QStringList wanted;
    for (int row = 0; row < panel.model()->rowCount(); ++row) {
        const FileInfo info = panel.model()->fileInfoAt(row);
        if (info.name() != QLatin1String("f1.txt") && info.name() != QLatin1String("f3.txt")
            && info.name() != QLatin1String("f5.txt"))
            continue;
        sel->select(panel.model()->index(row, 0), QItemSelectionModel::Select);
        wanted << info.path();
    }
    ASSERT_EQ(wanted.size(), 3);
    ASSERT_TRUE(sel->selectedRows().isEmpty())
        << "the premise: IconMode never produces a fully-selected row";

    EXPECT_EQ(panel.selectedPaths(), wanted);
    ASSERT_EQ(panel.selectedEntryInfos().size(), 3);
    // Ascending row order, so the copy lands in the order the panel showed.
    EXPECT_EQ(panel.selectedEntryInfos().at(0).name(), QStringLiteral("f1.txt"));
    EXPECT_EQ(panel.selectedEntryInfos().at(2).name(), QStringLiteral("f5.txt"));
}
