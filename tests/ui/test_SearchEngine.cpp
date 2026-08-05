#include <gtest/gtest.h>

#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHash>
#include <QMutex>
#include <QSemaphore>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <memory>
#include <thread>

#include "FileInfo.h"
#include "FileProvider.h"
#include "SearchEngine.h"

// The search engine has two traversals -- QDirIterator over the local
// filesystem and FileProvider::list() over a network backend -- and the point of
// these tests is that both obey the same contract. The local half runs against
// a real QTemporaryDir; the remote half runs against an in-memory provider,
// which is the honest way to cover the cases that matter (a directory tree that
// reaches itself, a server that never stops inventing subdirectories,
// cancelling while a listing is in flight) since no real server does those on
// demand.
namespace {

// Serves a fixed directory tree from memory. Records every path it was asked to
// list, and the peak number of list() calls that overlapped, so a test can
// assert both what was walked and that nothing walked it in parallel.
class TreeProvider : public FileProvider {
public:
    // Adds `name` under `dir`. `dir` must already be reachable.
    void addFile(const QString &dir, const QString &name) { add(dir, name, false, QString()); }
    void addDir(const QString &dir, const QString &name) { add(dir, name, true, QString()); }
    // A directory entry whose real path is somewhere else in the tree -- what a
    // server that resolves two names onto one directory looks like from here.
    void addDirAlias(const QString &dir, const QString &name, const QString &target) {
        add(dir, name, true, target);
    }

    void setListDelayMs(int ms) { m_listDelayMs = ms; }

    QStringList listed() const {
        QMutexLocker lock(&m_mutex);
        return m_listed;
    }
    int peakConcurrentLists() const { return m_peak.load(); }

    QVector<FileInfo> list(const QString &path, bool) const override {
        const int now = ++m_inList;
        int prev = m_peak.load();
        while (now > prev && !m_peak.compare_exchange_weak(prev, now)) {
        }
        if (m_listDelayMs > 0)
            QThread::msleep(m_listDelayMs);
        {
            QMutexLocker lock(&m_mutex);
            m_listed.append(path);
        }
        const QVector<FileInfo> out = m_tree.value(cleanPath(path));
        --m_inList;
        return out;
    }

    bool isDir(const QString &path) const override { return m_tree.contains(cleanPath(path)); }
    bool exists(const QString &path) const override { return true; }
    RenameResult rename(const QString &, const QString &, QString *) override {
        return RenameResult::Failed;
    }
    // Marks this as a network backend, matching how the app tells the two apart.
    QString displayName() const override { return QStringLiteral("fake@host"); }
    QString scheme() const override { return QStringLiteral("smb"); }

    QString cleanPath(const QString &path) const override {
        const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        return parts.isEmpty() ? QStringLiteral("/")
                               : QLatin1Char('/') + parts.join(QLatin1Char('/'));
    }
    QString parentPath(const QString &path) const override {
        const QString clean = cleanPath(path);
        const int slash = clean.lastIndexOf(QLatin1Char('/'));
        return slash <= 0 ? QStringLiteral("/") : clean.left(slash);
    }

private:
    void add(const QString &dir, const QString &name, bool isDir, const QString &target) {
        const QString parent = cleanPath(dir);
        const QString self = parent == QStringLiteral("/") ? QLatin1Char('/') + name
                                                           : parent + QLatin1Char('/') + name;
        const QString path = target.isEmpty() ? self : cleanPath(target);
        m_tree[parent].append(
            FileInfo::fromFields(path, name, 0, QDateTime(), isDir, QFile::Permissions()));
        if (isDir && target.isEmpty() && !m_tree.contains(self))
            m_tree.insert(self, {});
        if (!m_tree.contains(parent))
            m_tree.insert(parent, {});
    }

    QHash<QString, QVector<FileInfo>> m_tree{{QStringLiteral("/"), {}}};
    int m_listDelayMs = 0;
    mutable QMutex m_mutex;
    mutable QStringList m_listed;
    mutable std::atomic<int> m_inList{0};
    mutable std::atomic<int> m_peak{0};
};

// A backend that never runs out of tree: every directory contains one fresh
// subdirectory. Only the engine's depth cap can end a walk of this.
class BottomlessProvider : public TreeProvider {
public:
    QVector<FileInfo> list(const QString &path, bool) const override {
        ++m_calls;
        const QString clean = cleanPath(path);
        const QString child =
            (clean == QStringLiteral("/") ? QString() : clean) + QStringLiteral("/down");
        QVector<FileInfo> out;
        out.append(FileInfo::fromFields(child, QStringLiteral("down"), 0, QDateTime(), true,
                                        QFile::Permissions()));
        return out;
    }
    int calls() const { return m_calls.load(); }

private:
    mutable std::atomic<int> m_calls{0};
};

// Drives one search to completion and returns every hit, in arrival order.
// `duringSearch` (if given) runs once the first batch of results has landed --
// that is how the cancellation test stops a walk that is genuinely mid-flight
// rather than one that has already finished.
QVector<SearchHit> runSearch(SearchEngine &engine, const QString &root, const QString &pattern,
                             bool caseSensitive, bool includeSubdirs,
                             std::shared_ptr<FileProvider> provider = {},
                             const std::function<void(SearchEngine &)> &duringSearch = {}) {
    QVector<SearchHit> all;
    QEventLoop loop;
    bool fired = false;
    QObject::connect(&engine, &SearchEngine::resultsFound, &loop,
                     [&](const QVector<SearchHit> &hits) {
                         all += hits;
                         if (duringSearch && !fired) {
                             fired = true;
                             duringSearch(engine);
                         }
                     });
    QObject::connect(&engine, &SearchEngine::finished, &loop, &QEventLoop::quit);
    engine.start(root, pattern, caseSensitive, includeSubdirs, std::move(provider));
    loop.exec();
    return all;
}

QSet<QString> pathsOf(const QVector<SearchHit> &hits) {
    QSet<QString> out;
    for (const SearchHit &h : hits)
        out.insert(h.path);
    return out;
}

// Builds a small local tree: root/{a.txt, b.log, sub/{c.txt}}.
void buildLocalTree(const QString &root) {
    QDir dir(root);
    ASSERT_TRUE(dir.mkpath(QStringLiteral("sub")));
    for (const QString &rel : {QStringLiteral("a.txt"), QStringLiteral("b.log"),
                               QStringLiteral("sub/c.txt")}) {
        QFile f(dir.filePath(rel));
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("x");
    }
}

} // namespace

// --- Local traversal: must behave exactly as it did before providers existed.

TEST(SearchEngineLocalTest, FindsMatchesRecursivelyOnTheLocalFilesystem) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    buildLocalTree(tmp.path());

    SearchEngine engine;
    const QVector<SearchHit> hits = runSearch(engine, tmp.path(), QStringLiteral("*.txt"), false,
                                              /*includeSubdirs=*/true);

    const QSet<QString> paths = pathsOf(hits);
    EXPECT_EQ(paths.size(), 2);
    EXPECT_TRUE(paths.contains(tmp.path() + QStringLiteral("/a.txt")));
    EXPECT_TRUE(paths.contains(tmp.path() + QStringLiteral("/sub/c.txt")));
    EXPECT_FALSE(paths.contains(tmp.path() + QStringLiteral("/b.log")));
}

TEST(SearchEngineLocalTest, HonoursTheIncludeSubdirectoriesFlag) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    buildLocalTree(tmp.path());

    SearchEngine engine;
    const QSet<QString> paths = pathsOf(runSearch(engine, tmp.path(), QStringLiteral("*.txt"),
                                                  false, /*includeSubdirs=*/false));
    EXPECT_EQ(paths.size(), 1);
    EXPECT_TRUE(paths.contains(tmp.path() + QStringLiteral("/a.txt")));
}

TEST(SearchEngineLocalTest, ReportsWhetherEachHitIsADirectory) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    buildLocalTree(tmp.path());

    SearchEngine engine;
    const QVector<SearchHit> hits =
        runSearch(engine, tmp.path(), QStringLiteral("sub"), false, true);
    ASSERT_EQ(hits.size(), 1);
    EXPECT_TRUE(hits.first().isDir);
}

// A symlink loop is the everyday way a local tree becomes infinite. The walk
// must terminate; QDirIterator's default (don't follow symlinks) is what makes
// it so, and this pins that we never turn FollowSymlinks on.
TEST(SearchEngineLocalTest, DoesNotRecurseThroughASymlinkLoop) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    buildLocalTree(tmp.path());
    if (!QFile::link(tmp.path(), tmp.path() + QStringLiteral("/loop")))
        GTEST_SKIP() << "filesystem does not support symlinks";

    SearchEngine engine;
    QElapsedTimer timer;
    timer.start();
    const QSet<QString> paths =
        pathsOf(runSearch(engine, tmp.path(), QStringLiteral("*.txt"), false, true));
    EXPECT_LT(timer.elapsed(), 5000);
    EXPECT_EQ(paths.size(), 2); // the loop contributed nothing extra
}

// --- Provider traversal: the actual bug. A network tab's paths do not exist
// locally, so QDirIterator returned nothing at all for them.

TEST(SearchEngineProviderTest, FindsMatchesThroughTheProviderRatherThanTheLocalFilesystem) {
    auto provider = std::make_shared<TreeProvider>();
    provider->addFile(QStringLiteral("/"), QStringLiteral("top.txt"));
    provider->addDir(QStringLiteral("/"), QStringLiteral("docs"));
    provider->addFile(QStringLiteral("/docs"), QStringLiteral("deep.txt"));
    provider->addFile(QStringLiteral("/docs"), QStringLiteral("notes.md"));

    SearchEngine engine;
    const QVector<SearchHit> hits =
        runSearch(engine, QStringLiteral("/"), QStringLiteral("*.txt"), false, true, provider);

    const QSet<QString> paths = pathsOf(hits);
    EXPECT_EQ(paths.size(), 2);
    EXPECT_TRUE(paths.contains(QStringLiteral("/top.txt")));
    EXPECT_TRUE(paths.contains(QStringLiteral("/docs/deep.txt")));
    // The pre-fix behaviour: none of these paths exist on this machine, so a
    // local walk of the same root is the empty set.
    EXPECT_FALSE(QDir(QStringLiteral("/docs")).exists());
}

TEST(SearchEngineProviderTest, SearchesOnlyTheRootWhenSubdirectoriesAreExcluded) {
    auto provider = std::make_shared<TreeProvider>();
    provider->addFile(QStringLiteral("/"), QStringLiteral("top.txt"));
    provider->addDir(QStringLiteral("/"), QStringLiteral("docs"));
    provider->addFile(QStringLiteral("/docs"), QStringLiteral("deep.txt"));

    SearchEngine engine;
    const QSet<QString> paths = pathsOf(runSearch(engine, QStringLiteral("/"),
                                                  QStringLiteral("*.txt"), false,
                                                  /*includeSubdirs=*/false, provider));
    EXPECT_EQ(paths, QSet<QString>{QStringLiteral("/top.txt")});
    EXPECT_EQ(provider->listed(), QStringList{QStringLiteral("/")});
}

TEST(SearchEngineProviderTest, ReportsWhetherEachHitIsADirectory) {
    auto provider = std::make_shared<TreeProvider>();
    provider->addDir(QStringLiteral("/"), QStringLiteral("report"));
    provider->addFile(QStringLiteral("/"), QStringLiteral("report.txt"));

    SearchEngine engine;
    const QVector<SearchHit> hits =
        runSearch(engine, QStringLiteral("/"), QStringLiteral("report*"), false, true, provider);
    ASSERT_EQ(hits.size(), 2);
    QHash<QString, bool> isDir;
    for (const SearchHit &h : hits)
        isDir.insert(h.path, h.isDir);
    EXPECT_TRUE(isDir.value(QStringLiteral("/report")));
    EXPECT_FALSE(isDir.value(QStringLiteral("/report.txt")));
}

// Matching is on the entry name, so a pattern must not accidentally match
// because some ancestor directory happens to contain the text.
TEST(SearchEngineProviderTest, MatchesTheEntryNameNotTheWholePath) {
    auto provider = std::make_shared<TreeProvider>();
    provider->addDir(QStringLiteral("/"), QStringLiteral("photos"));
    provider->addFile(QStringLiteral("/photos"), QStringLiteral("readme.md"));

    SearchEngine engine;
    const QSet<QString> paths = pathsOf(
        runSearch(engine, QStringLiteral("/"), QStringLiteral("photos"), false, true, provider));
    EXPECT_EQ(paths, QSet<QString>{QStringLiteral("/photos")});
}

TEST(SearchEngineProviderTest, CaseSensitivityIsHonoured) {
    auto provider = std::make_shared<TreeProvider>();
    provider->addFile(QStringLiteral("/"), QStringLiteral("Report.TXT"));

    SearchEngine sensitive;
    EXPECT_TRUE(pathsOf(runSearch(sensitive, QStringLiteral("/"), QStringLiteral("*.txt"),
                                  /*caseSensitive=*/true, true, provider))
                    .isEmpty());

    SearchEngine insensitive;
    EXPECT_EQ(pathsOf(runSearch(insensitive, QStringLiteral("/"), QStringLiteral("*.txt"),
                                /*caseSensitive=*/false, true, provider))
                  .size(),
              1);
}

// A server can resolve two names onto one directory (a DFS referral pointing
// back up the tree). The normalised visited-set has to notice, or the walk
// spins forever.
TEST(SearchEngineProviderTest, StopsWhenTheTreeReachesItself) {
    auto provider = std::make_shared<TreeProvider>();
    provider->addDir(QStringLiteral("/"), QStringLiteral("a"));
    provider->addFile(QStringLiteral("/a"), QStringLiteral("hit.txt"));
    // "/a/back" is really "/a" again.
    provider->addDirAlias(QStringLiteral("/a"), QStringLiteral("back"), QStringLiteral("/a"));

    SearchEngine engine;
    QElapsedTimer timer;
    timer.start();
    const QSet<QString> paths =
        pathsOf(runSearch(engine, QStringLiteral("/"), QStringLiteral("*.txt"), false, true,
                          provider));
    EXPECT_LT(timer.elapsed(), 5000);
    EXPECT_EQ(paths, QSet<QString>{QStringLiteral("/a/hit.txt")});
    // "/" and "/a", and nothing twice.
    const QStringList listed = provider->listed();
    EXPECT_EQ(listed.size(), 2);
    EXPECT_EQ(QSet<QString>(listed.begin(), listed.end()).size(), 2);
}

// The visited-set cannot help when every level has a genuinely new name; the
// depth cap is the backstop, and this is what proves it holds.
TEST(SearchEngineProviderTest, DepthCapEndsAnEndlesslyDeepTree) {
    auto provider = std::make_shared<BottomlessProvider>();

    SearchEngine engine;
    QElapsedTimer timer;
    timer.start();
    runSearch(engine, QStringLiteral("/"), QStringLiteral("nothing-matches"), false, true,
              provider);
    EXPECT_LT(timer.elapsed(), 10000);
    // The root plus one listing per level down to the cap.
    EXPECT_LE(provider->calls(), SearchEngine::kMaxDepth + 1);
    EXPECT_GT(provider->calls(), 1);
}

// Every backend serialises list() behind its own connection mutex, so walking
// in parallel would only queue on that mutex while starving the browsing
// session. Assert the engine issues one listing at a time.
TEST(SearchEngineProviderTest, ListsOneDirectoryAtATime) {
    auto provider = std::make_shared<TreeProvider>();
    provider->addDir(QStringLiteral("/"), QStringLiteral("a"));
    provider->addDir(QStringLiteral("/"), QStringLiteral("b"));
    provider->addDir(QStringLiteral("/"), QStringLiteral("c"));
    for (const QString &d : {QStringLiteral("/a"), QStringLiteral("/b"), QStringLiteral("/c")})
        provider->addFile(d, QStringLiteral("f.txt"));
    provider->setListDelayMs(20);

    SearchEngine engine;
    runSearch(engine, QStringLiteral("/"), QStringLiteral("*.txt"), false, true, provider);
    EXPECT_EQ(provider->peakConcurrentLists(), 1);
}

// Cancellation on a slow share must take effect after the listing in flight,
// not after the whole tree. The walk below would need every one of 40
// directories; stopping at the first result has to cut that short.
TEST(SearchEngineProviderTest, CancelStopsTheWalkWithoutDrainingTheTree) {
    auto provider = std::make_shared<TreeProvider>();
    constexpr int kDirs = 40;
    for (int i = 0; i < kDirs; ++i) {
        const QString name = QStringLiteral("d%1").arg(i);
        provider->addDir(QStringLiteral("/"), name);
        provider->addFile(QLatin1Char('/') + name, QStringLiteral("hit.txt"));
    }
    provider->setListDelayMs(10);

    SearchEngine engine;
    runSearch(engine, QStringLiteral("/"), QStringLiteral("hit.txt"), false, true, provider,
              [](SearchEngine &e) { e.cancel(); });

    EXPECT_FALSE(engine.isRunning());
    // It got nowhere near the whole tree. (The exact count is timing-dependent:
    // cancel() is delivered from the GUI thread while the worker is inside a
    // listing, so a few more may land before it is observed.)
    EXPECT_LT(provider->listed().size(), kDirs / 2);
}

TEST(SearchEngineProviderTest, EmitsScanningProgressForTheDirectoriesItWalks) {
    auto provider = std::make_shared<TreeProvider>();
    provider->addDir(QStringLiteral("/"), QStringLiteral("deep"));
    provider->addFile(QStringLiteral("/deep"), QStringLiteral("x.txt"));

    SearchEngine engine;
    QSignalSpy spy(&engine, &SearchEngine::scanning);
    runSearch(engine, QStringLiteral("/"), QStringLiteral("*.txt"), false, true, provider);
    // At least the first directory is always reported, so the status line has
    // something to show the moment a slow search begins.
    ASSERT_GE(spy.count(), 1);
    EXPECT_EQ(spy.first().first().toString(), QStringLiteral("/"));
}

// A local search stays on the QDirIterator path and must not emit the
// per-directory progress (it would be thousands of queued signals for no gain).
TEST(SearchEngineLocalTest, DoesNotEmitScanningProgress) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    buildLocalTree(tmp.path());

    SearchEngine engine;
    QSignalSpy spy(&engine, &SearchEngine::scanning);
    runSearch(engine, tmp.path(), QStringLiteral("*"), false, true);
    EXPECT_EQ(spy.count(), 0);
}

// Destroying an engine mid-search must leave no thread behind.
//
// This is the test for a segfault that hit about one full ui_tests run in four
// and never in the same place twice: a SearchDialogKeysTest case started a
// search, the fixture destroyed the engine at the end of the test, and the
// still-running worker then emitted resultsFound through a freed QObject --
// which crashed inside QMetaObject::activate during whatever unrelated test was
// running by then (DragPayloadTest, several suites later, in the run that was
// finally caught with a stack). The same sequence is reachable from the app:
// close the search window, or the window that owns it, while a search runs.
//
// Deliberately not written as "and the process does not crash": a
// use-after-free is not required to crash, so that test would pass for the
// wrong reason on most runs -- and did, on Windows, where the sabotaged build
// came through cleanly.
//
// The worker is pinned inside a listing by a gate rather than by a sleep, so
// "the engine was destroyed WHILE a listing was in flight" is a fact rather
// than a race the test hopes to win. An earlier version used TreeProvider's
// delay for this and was itself flaky: that provider sleeps before it records
// the listing, so by the time the test could see a listing had begun, the
// worker was already on its way out of it.
namespace {

class GatedProvider : public TreeProvider {
public:
    // Blocks the walk inside its first listing until openGate() is called.
    QVector<FileInfo> list(const QString &path, bool includeHidden) const override {
        const QVector<FileInfo> out = TreeProvider::list(path, includeHidden);
        if (!m_entered.load()) {
            m_entered = true;
            m_gate.acquire();
        }
        return out;
    }

    bool waitUntilInsideAListing(int budgetMs = 5000) const {
        QElapsedTimer timer;
        timer.start();
        while (!m_entered.load() && timer.elapsed() < budgetMs)
            QThread::msleep(5);
        return m_entered.load();
    }
    void openGate() const { m_gate.release(); }

private:
    mutable std::atomic<bool> m_entered{false};
    mutable QSemaphore m_gate;
};

} // namespace

TEST(SearchEngineTest, DestroyingTheEngineMidSearchWaitsForTheWorker) {
    auto provider = std::make_shared<GatedProvider>();
    for (int i = 0; i < 12; ++i) {
        const QString dir = QStringLiteral("d%1").arg(i);
        provider->addDir(QStringLiteral("/"), dir);
        provider->addFile(QStringLiteral("/") + dir, QStringLiteral("match.txt"));
    }

    auto engine = std::make_unique<SearchEngine>();
    engine->start(QStringLiteral("/"), QStringLiteral("*.txt"), false, true, provider);
    ASSERT_TRUE(provider->waitUntilInsideAListing()) << "the search never reached a listing";

    // Held open for a stretch the destructor cannot cross by accident. Released
    // from another thread because the thread that would normally do it -- this
    // one -- is about to be blocked inside ~SearchEngine, which is the whole
    // point.
    constexpr int kHoldMs = 300;
    std::thread opener([provider, kHoldMs]() {
        QThread::msleep(kHoldMs);
        provider->openGate();
    });

    QElapsedTimer destruction;
    destruction.start();
    engine.reset();
    const qint64 blockedFor = destruction.elapsed();
    opener.join();

    // The worker was inside a listing that could not return for kHoldMs, so a
    // destructor that waited for it cannot have come back sooner. One that
    // merely set the cancel flag returns immediately.
    EXPECT_GE(blockedFor, kHoldMs / 2)
        << "the destructor returned while the worker was still inside a listing";

    // And nothing was left running: whatever the walk had listed when the
    // destructor returned is all it ever lists.
    const int listedAtDestruction = provider->listed().size();
    QThread::msleep(200);
    EXPECT_EQ(provider->listed().size(), listedAtDestruction)
        << "the walk kept listing after ~SearchEngine returned";
}
