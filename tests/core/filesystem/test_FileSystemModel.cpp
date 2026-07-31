#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTest>
#include <QtTest/QSignalSpy>
#include <QTemporaryDir>

#include <algorithm>
#include <condition_variable>
#include <memory>
#include <mutex>

#include "FileListView.h"
#include "FilePanel.h"
#include "FileProvider.h"
#include "FileSystemModel.h"

namespace {

void touch(const QString &dir, const QString &name) {
    QFile f(QDir(dir).filePath(name));
    f.open(QIODevice::WriteOnly);
    f.write("x");
    f.close();
}

void writeBytes(const QString &path, int n) {
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(QByteArray(n, 'x'));
    f.close();
}

// Drives the model's async scan to completion.
bool loadDir(FileSystemModel &model, const QString &path) {
    QSignalSpy spy(&model, &FileSystemModel::loadFinished);
    model.setRootPath(path);
    return spy.wait(2000) || spy.count() > 0;
}

// Visible real entries, excluding the synthesized ".." parent row (a temp
// dir always has a parent, so rowCount includes it).
int visibleFiles(const FileSystemModel &model) {
    int rows = model.rowCount();
    if (rows > 0 && model.isParentEntry(0))
        --rows;
    return rows;
}

class StaleSizeProvider final : public FileProvider {
public:
    QVector<FileInfo> list(const QString &path, bool) const override {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (path == QStringLiteral("/")) {
            return {FileInfo::fromFields(QStringLiteral("/first"), QStringLiteral("first"), 0,
                                         {}, true, {}),
                    FileInfo::fromFields(QStringLiteral("/second"), QStringLiteral("second"), 0,
                                         {}, true, {}),
                    FileInfo::fromFields(QStringLiteral("/third"), QStringLiteral("third"), 0,
                                         {}, true, {})};
        }
        ++m_activeRequests;
        m_maxConcurrentRequests = std::max(m_maxConcurrentRequests, m_activeRequests);
        m_requestedPaths.append(path);
        QVector<FileInfo> result;
        if (path == QStringLiteral("/first")) {
            m_firstRequestStarted = true;
            m_firstRequest.notify_all();
            m_releaseFirst.wait(lock, [this] { return m_firstRequestReleased; });
            result = {FileInfo::fromFields(QStringLiteral("/first/old.bin"),
                                           QStringLiteral("old.bin"), 10, {}, false, {})};
        } else if (path == QStringLiteral("/second")) {
            result = {FileInfo::fromFields(QStringLiteral("/second/new.bin"),
                                           QStringLiteral("new.bin"), 20, {}, false, {})};
        } else if (path == QStringLiteral("/third")) {
            result = {FileInfo::fromFields(QStringLiteral("/third/latest.bin"),
                                           QStringLiteral("latest.bin"), 30, {}, false, {})};
        }
        --m_activeRequests;
        return result;
    }

    bool isDir(const QString &) const override { return true; }
    QString cleanPath(const QString &path) const override { return path; }
    QString parentPath(const QString &) const override { return {}; }
    bool exists(const QString &) const override { return true; }
    RenameResult rename(const QString &, const QString &, QString *) override {
        return RenameResult::Failed;
    }

    void waitUntilFirstRequestStarted() const {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_firstRequest.wait(lock, [this] { return m_firstRequestStarted; });
    }

    void releaseFirstRequest() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_firstRequestReleased = true;
        m_releaseFirst.notify_all();
    }

    int maxConcurrentRequests() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_maxConcurrentRequests;
    }

    QStringList requestedPaths() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_requestedPaths;
    }

private:
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_firstRequest;
    mutable std::condition_variable m_releaseFirst;
    mutable bool m_firstRequestStarted = false;
    mutable bool m_firstRequestReleased = false;
    mutable int m_activeRequests = 0;
    mutable int m_maxConcurrentRequests = 0;
    mutable QStringList m_requestedPaths;
};

bool loadPanel(FilePanel &panel, const QString &path) {
    QSignalSpy spy(panel.model(), &FileSystemModel::loadFinished);
    panel.navigateTo(path);
    return spy.wait(4000) || spy.count() > 0;
}

} // namespace

TEST(FileSystemModelFilterTest, MatchesFilterIsCaseInsensitiveSubstring) {
    EXPECT_TRUE(FileSystemModel::matchesFilter("Report.txt", "report"));
    EXPECT_TRUE(FileSystemModel::matchesFilter("Report.txt", "PORT"));
    EXPECT_FALSE(FileSystemModel::matchesFilter("Report.txt", "xyz"));
}

TEST(FileSystemModelFilterTest, MatchesFilterEmptyMatchesEverything) {
    EXPECT_TRUE(FileSystemModel::matchesFilter("anything", ""));
}

TEST(FileSystemModelFilterTest, MatchesFilterSupportsWildcards) {
    EXPECT_TRUE(FileSystemModel::matchesFilter("photo.png", "*.png"));
    EXPECT_FALSE(FileSystemModel::matchesFilter("photo.png", "*.jpg"));
    EXPECT_TRUE(FileSystemModel::matchesFilter("a1b.txt", "a?b*"));
}

TEST(FileSystemModelFilterTest, SetNameFilterRestrictsVisibleRows) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    touch(dir.path(), "alpha.txt");
    touch(dir.path(), "beta.txt");
    touch(dir.path(), "alphabet.md");

    FileSystemModel model;
    ASSERT_TRUE(loadDir(model, dir.path()));
    EXPECT_EQ(visibleFiles(model), 3);

    model.setNameFilter("alpha");
    EXPECT_EQ(visibleFiles(model), 2); // alpha.txt + alphabet.md

    model.setNameFilter("*.txt");
    EXPECT_EQ(visibleFiles(model), 2); // alpha.txt + beta.txt

    model.setNameFilter("");
    EXPECT_EQ(visibleFiles(model), 3);
}

TEST(FileSystemModelFilterTest, PatternPicksExpectedEntriesAcrossRows) {
    // Mirrors FilePanel::selectByPattern: iterate the model's rows and match
    // each name against a wildcard mask.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    touch(dir.path(), "notes.txt");
    touch(dir.path(), "todo.txt");
    touch(dir.path(), "image.png");

    FileSystemModel model;
    ASSERT_TRUE(loadDir(model, dir.path()));

    QStringList matched;
    for (int r = 0; r < model.rowCount(); ++r) {
        if (model.isParentEntry(r))
            continue;
        const QString name = model.fileInfoAt(r).name();
        if (FileSystemModel::matchesFilter(name, "*.txt"))
            matched << name;
    }
    matched.sort();
    EXPECT_EQ(matched, (QStringList{"notes.txt", "todo.txt"}));
}

TEST(FileSystemModelCompareTest, ClassifiesUniqueNewerAndOlder) {
    const QDateTime t1 = QDateTime::fromSecsSinceEpoch(1000);
    const QDateTime t2 = QDateTime::fromSecsSinceEpoch(2000);

    QHash<QString, QDateTime> self{{"a.txt", t2}, {"b.txt", t1}, {"only.txt", t1}};
    QHash<QString, QDateTime> other{{"a.txt", t1}, {"b.txt", t2}, {"elsewhere.txt", t1}};

    const QHash<QString, int> status = FileSystemModel::compareStatuses(self, other);
    EXPECT_EQ(status.value("a.txt"), FileSystemModel::CompareNewer);   // t2 > t1
    EXPECT_EQ(status.value("b.txt"), FileSystemModel::CompareOlder);   // t1 < t2
    EXPECT_EQ(status.value("only.txt"), FileSystemModel::CompareUnique);
    EXPECT_FALSE(status.contains("elsewhere.txt")); // only classifies self's entries
}

TEST(FileSystemModelTypeTest, CategorizesByExtension) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    touch(dir.path(), "photo.PNG"); // case-insensitive
    touch(dir.path(), "clip.mp4");
    touch(dir.path(), "paper.pdf");
    touch(dir.path(), "bundle.zip");
    touch(dir.path(), "script.xyz");
    ASSERT_TRUE(QDir(dir.path()).mkdir("folder"));

    auto cat = [&](const QString &name) {
        return FileSystemModel::typeCategory(FileInfo(QDir(dir.path()).filePath(name)))
            .toStdString();
    };
    // English sources (no .qm loaded in the test).
    EXPECT_EQ(cat("photo.PNG"), "Image");
    EXPECT_EQ(cat("clip.mp4"), "Video");
    EXPECT_EQ(cat("paper.pdf"), "Document");
    EXPECT_EQ(cat("bundle.zip"), "Archive");
    EXPECT_EQ(cat("folder"), "Folder");
    EXPECT_EQ(cat("script.xyz"), "XYZ"); // unknown -> uppercased extension
}

TEST(FileSystemModelSizeTest, DirectorySizeSumsRecursively) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    writeBytes(QDir(dir.path()).filePath("a.bin"), 100);
    ASSERT_TRUE(QDir(dir.path()).mkdir("sub"));
    writeBytes(QDir(dir.path()).filePath("sub/b.bin"), 200);

    EXPECT_EQ(FileSystemModel::directorySize(dir.path()), 300);
}

TEST(FileSystemModelSizeTest, ComputedSizeReplacesDirPlaceholderInSizeColumn) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir(dir.path()).mkdir("folder"));
    writeBytes(QDir(dir.path()).filePath("folder/inner.bin"), 2048);

    FileSystemModel model;
    ASSERT_TRUE(loadDir(model, dir.path()));

    // Locate the row for "folder".
    int folderRow = -1;
    for (int r = 0; r < model.rowCount(); ++r) {
        if (model.fileInfoAt(r).name() == "folder") {
            folderRow = r;
            break;
        }
    }
    ASSERT_GE(folderRow, 0);

    const QModelIndex sizeIdx = model.index(folderRow, FileSystemModel::SizeColumn);
    EXPECT_EQ(model.data(sizeIdx).toString().toStdString(), "<DIR>");

    model.setComputedDirSize(QDir(dir.path()).filePath("folder"), 2048);
    EXPECT_EQ(model.data(sizeIdx).toString().toStdString(), "2.0 KB");
}

TEST(FileSystemModelSizeTest, CalculatingMarkerIsReplacedByComputedSize) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir(dir.path()).mkdir("folder"));

    FileSystemModel model;
    ASSERT_TRUE(loadDir(model, dir.path()));

    int folderRow = -1;
    for (int r = 0; r < model.rowCount(); ++r) {
        if (model.fileInfoAt(r).name() == "folder") {
            folderRow = r;
            break;
        }
    }
    ASSERT_GE(folderRow, 0);

    const QString folderPath = QDir(dir.path()).filePath("folder");
    const QModelIndex sizeIdx = model.index(folderRow, FileSystemModel::SizeColumn);
    model.setDirectorySizeCalculating(folderPath, true);
    EXPECT_EQ(model.data(sizeIdx).toString(), QStringLiteral("计算中"));

    model.setComputedDirSize(folderPath, 4096);
    EXPECT_EQ(model.data(sizeIdx).toString(), QStringLiteral("4.0 KB"));
}

TEST(FilePanelDirectorySize, SlowRequestShowsCalculatingMarkerUntilFinished) {
    auto provider = std::make_shared<StaleSizeProvider>();
    FilePanel panel;
    panel.model()->setProvider(provider);
    ASSERT_TRUE(loadPanel(panel, QStringLiteral("/")));

    FileListView *view = panel.findChild<FileListView *>();
    ASSERT_NE(view, nullptr);
    const QModelIndex first = panel.model()->index(0, FileSystemModel::NameColumn);
    const QModelIndex second = panel.model()->index(1, FileSystemModel::NameColumn);
    view->setCurrentIndex(first);
    panel.calculateDirSizes();
    provider->waitUntilFirstRequestStarted();

    const QModelIndex sizeIdx = panel.model()->index(0, FileSystemModel::SizeColumn);
    EXPECT_EQ(panel.model()->data(sizeIdx).toString(), QStringLiteral("<DIR>"));
    QTRY_COMPARE_WITH_TIMEOUT(panel.model()->data(sizeIdx).toString(),
                              QStringLiteral("计算中"), 2000);

    view->setCurrentIndex(second);
    QTest::qWait(50);
    EXPECT_EQ(panel.model()->data(sizeIdx).toString(), QStringLiteral("计算中"));

    provider->releaseFirstRequest();
    QTRY_COMPARE_WITH_TIMEOUT(panel.model()->data(sizeIdx).toString(),
                              QStringLiteral("10 B"), 4000);
}

TEST(FileSystemModelFilterTest, ChangingDirectoryClearsFilter) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    touch(dir.path(), "one.txt");
    touch(dir.path(), "two.log");

    FileSystemModel model;
    ASSERT_TRUE(loadDir(model, dir.path()));
    model.setNameFilter("one");
    EXPECT_EQ(visibleFiles(model), 1);

    // Re-scanning (navigation / refresh) must reset the filter.
    ASSERT_TRUE(loadDir(model, dir.path()));
    EXPECT_TRUE(model.nameFilter().isEmpty());
    EXPECT_EQ(visibleFiles(model), 2);
}

TEST(FilePanelDirectorySize, OlderRequestCannotOverwriteNewerSelection) {
    auto provider = std::make_shared<StaleSizeProvider>();
    FilePanel panel;
    panel.model()->setProvider(provider);
    ASSERT_TRUE(loadPanel(panel, QStringLiteral("/")));

    FileListView *view = panel.findChild<FileListView *>();
    ASSERT_NE(view, nullptr);
    const QModelIndex first = panel.model()->index(0, FileSystemModel::NameColumn);
    const QModelIndex second = panel.model()->index(1, FileSystemModel::NameColumn);
    view->setCurrentIndex(first);
    panel.calculateDirSizes();
    provider->waitUntilFirstRequestStarted();

    view->setCurrentIndex(second);
    panel.calculateDirSizes();
    provider->releaseFirstRequest();

    QTRY_COMPARE_WITH_TIMEOUT(
        panel.model()->data(panel.model()->index(1, FileSystemModel::SizeColumn)).toString(),
        QStringLiteral("20 B"), 4000);
    EXPECT_EQ(panel.model()->data(panel.model()->index(0, FileSystemModel::SizeColumn)).toString(),
              QStringLiteral("<DIR>"));
}

TEST(FilePanelDirectorySize, RemoteReplacementsAreSerializedAndCoalescedToNewest) {
    auto provider = std::make_shared<StaleSizeProvider>();
    FilePanel panel;
    panel.model()->setProvider(provider);
    ASSERT_TRUE(loadPanel(panel, QStringLiteral("/")));

    FileListView *view = panel.findChild<FileListView *>();
    ASSERT_NE(view, nullptr);
    const QModelIndex first = panel.model()->index(0, FileSystemModel::NameColumn);
    const QModelIndex second = panel.model()->index(1, FileSystemModel::NameColumn);
    const QModelIndex third = panel.model()->index(2, FileSystemModel::NameColumn);

    view->setCurrentIndex(first);
    panel.calculateDirSizes();
    provider->waitUntilFirstRequestStarted();

    view->setCurrentIndex(second);
    panel.calculateDirSizes();
    view->setCurrentIndex(third);
    panel.calculateDirSizes();

    QTest::qWait(100);
    EXPECT_EQ(provider->maxConcurrentRequests(), 1);
    EXPECT_EQ(provider->requestedPaths(), QStringList{QStringLiteral("/first")});

    provider->releaseFirstRequest();
    QTRY_COMPARE_WITH_TIMEOUT(
        panel.model()->data(panel.model()->index(2, FileSystemModel::SizeColumn)).toString(),
        QStringLiteral("30 B"), 4000);

    EXPECT_EQ(provider->maxConcurrentRequests(), 1);
    EXPECT_EQ(provider->requestedPaths(),
              (QStringList{QStringLiteral("/first"), QStringLiteral("/third")}));
    EXPECT_EQ(panel.model()->data(panel.model()->index(0, FileSystemModel::SizeColumn)).toString(),
              QStringLiteral("<DIR>"));
    EXPECT_EQ(panel.model()->data(panel.model()->index(1, FileSystemModel::SizeColumn)).toString(),
              QStringLiteral("<DIR>"));
}

TEST(FilePanelDirectorySize, SymlinkRootUsesListingMetadataWithoutTraversingTarget) {
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString target = QDir(temp.path()).filePath(QStringLiteral("target"));
    const QString link = QDir(temp.path()).filePath(QStringLiteral("link"));
    ASSERT_TRUE(QDir().mkdir(target));

    QFile payload(QDir(target).filePath(QStringLiteral("payload.bin")));
    ASSERT_TRUE(payload.open(QIODevice::WriteOnly));
    ASSERT_EQ(payload.write(QByteArray(4096, 'x')), 4096);
    payload.close();

    if (!QFile::link(target, link))
        GTEST_SKIP() << "filesystem does not support directory links";
    const FileInfo linkInfo(link);
    if (!linkInfo.isSymLink() || !linkInfo.isDir())
        GTEST_SKIP() << "platform does not expose directory links as directory symlinks";
    ASSERT_LT(linkInfo.size(), 1024);

    FilePanel panel;
    ASSERT_TRUE(loadPanel(panel, temp.path()));
    FileListView *view = panel.findChild<FileListView *>();
    ASSERT_NE(view, nullptr);

    int linkRow = -1;
    for (int row = 0; row < panel.model()->rowCount(); ++row) {
        if (panel.model()->fileInfoAt(row).path() == link) {
            linkRow = row;
            break;
        }
    }
    ASSERT_GE(linkRow, 0);

    view->setCurrentIndex(panel.model()->index(linkRow, FileSystemModel::NameColumn));
    panel.calculateDirSizes();

    const QString expected = QStringLiteral("%1 B").arg(linkInfo.size());
    QTRY_COMPARE_WITH_TIMEOUT(
        panel.model()->data(panel.model()->index(linkRow, FileSystemModel::SizeColumn)).toString(),
        expected, 4000);
}
