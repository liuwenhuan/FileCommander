#include <gtest/gtest.h>

#include "ImagePreviewLoader.h"
#include "ThumbnailCache.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QSemaphore>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include <iostream>
#include <memory>

namespace {

constexpr auto kImagePreviewShutdownProbe = "--image-preview-shutdown-probe";

// Gives every test its own thumbnail cache root, and lets the previous test's
// background work finish before that root goes away.
//
// ThumbnailCache is a process-wide singleton with a worker pool behind it, and
// its stored bitmaps all landed in one directory -- shared by every test in the
// run and, since the directory outlives the process, by every earlier run too.
// Tests that count files in it were therefore reading their neighbours' output
// and their own leftovers, so an unchanged binary produced a different set of
// failures on each run. That is worse than a plain bug: it costs a re-run and a
// hand-triage before any full-suite result can be believed, which is exactly
// when a real regression gets waved through as "probably the flaky ones".
//
// Draining at the END of each test is the half that is easy to miss. A worker
// derives its output path when it finishes rather than when it starts, so a
// straggler left running would write into whichever directory is current by
// then -- the next test's. Waiting here also keeps that test's decoding off the
// next one's CPU, which is what the wall-clock assertions elsewhere in the
// suite (animation progress, "renders in under a second") are measuring.
//
// Both hooks report trouble on stderr rather than through ADD_FAILURE: they run
// outside any test body, where a recorded failure has no test to belong to.
class ThumbnailCacheIsolation : public ::testing::EmptyTestEventListener {
public:
    void OnTestStart(const ::testing::TestInfo &info) override {
        m_root = std::make_unique<QTemporaryDir>();
        const QString dir = QDir(m_root->path()).filePath(QStringLiteral("thumbnails"));
        if (!m_root->isValid() || !QDir().mkpath(dir)) {
            std::cerr << "[ isolation] no cache root for " << info.test_suite_name() << '.'
                      << info.name() << ": " << qPrintable(dir) << std::endl;
            return;
        }
        ThumbnailCache::setCacheDirectoryForTest(dir);
    }

    void OnTestEnd(const ::testing::TestInfo &info) override {
        waitForIdle(info);
        // The decoded-pixmap LRU is process-wide too, and its budget is
        // something tests move about; both are put back the way a fresh process
        // would have them.
        ThumbnailCache::instance().setMemoryBudgetKiBForTest(ThumbnailCache::kMemoryBudgetKiB);
        // The override is deliberately left pointing at the directory about to
        // be deleted rather than restored to the standard location: anything
        // that somehow outlives the wait above should fail to write, not reach
        // the developer's real cache.
        m_root.reset();
    }

private:
    // Bounded rather than unbounded: a wedged fetch must not hang the whole
    // suite. Overrunning the budget is announced, because the isolation is only
    // as good as this wait and silence would make a leak look like a pass.
    static void waitForIdle(const ::testing::TestInfo &info, int budgetMs = 15000) {
        QElapsedTimer timer;
        timer.start();
        while (ThumbnailCache::instance().inFlightCountForTest() > 0) {
            if (timer.elapsed() >= budgetMs) {
                std::cerr << "[ isolation] " << info.test_suite_name() << '.' << info.name()
                          << " left " << ThumbnailCache::instance().inFlightCountForTest()
                          << " thumbnail jobs in flight after " << budgetMs << " ms"
                          << std::endl;
                return;
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        }
    }

    std::unique_ptr<QTemporaryDir> m_root;
};

int runImagePreviewShutdownProbe(QApplication &app) {
    struct Blockers {
        QSemaphore entered;
        QSemaphore release;
    };
    auto *blockers = new Blockers;

    QTemporaryDir dir;
    if (!dir.isValid())
        return 2;
    const QString path = QDir(dir.path()).filePath(QStringLiteral("shutdown.png"));
    QImage diskImage(QSize(80, 50), QImage::Format_ARGB32);
    diskImage.fill(Qt::red);
    if (!diskImage.save(path, "PNG"))
        return 3;

    auto loader = std::make_unique<ImagePreviewLoader>();
    loader->setWorkerCheckpointForTest(
        [blockers](ImagePreviewLoader::WorkerCheckpoint checkpoint, quint64) {
            if (checkpoint == ImagePreviewLoader::WorkerCheckpoint::LoadBeforeDecode ||
                checkpoint == ImagePreviewLoader::WorkerCheckpoint::RenderBeforeTransform ||
                checkpoint == ImagePreviewLoader::WorkerCheckpoint::RotationBeforeWrite) {
                blockers->entered.release();
                blockers->release.acquire();
            }
        });

    loader->load(path);
    if (!blockers->entered.tryAcquire(1, 5000))
        return 4;
    loader->render(diskImage, QSize(40, 25), QTransform());
    if (!blockers->entered.tryAcquire(1, 5000))
        return 5;
    loader->persistRotation(path, 90);
    if (!blockers->entered.tryAcquire(1, 5000))
        return 6;

    loader.reset();
    QTimer::singleShot(0, &app, &QCoreApplication::quit);
    return app.exec();
}

} // namespace

// The suite defaults to offscreen for headless CI. A caller can explicitly set
// QT_QPA_PLATFORM=xcb (with DISPLAY=:1) for real X11 geometry and theme tests.
int main(int argc, char **argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("FILECOMMANDER_DISABLE_ANIMATIONS", "1");
    // Every path QStandardPaths hands out moves to a throwaway test location for
    // the whole run, so config and session state written by a test cannot reach
    // the developer's real files. (The thumbnail cache no longer relies on this
    // -- ThumbnailCacheIsolation gives each test a directory of its own -- but
    // everything else built on QStandardPaths still does.)
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    if (app.arguments().contains(QLatin1String(kImagePreviewShutdownProbe)))
        return runImagePreviewShutdownProbe(app);

    // Construct the cache singleton here, against a root of its own, because
    // constructing it purges the cache directory (see purgeIfStale()). Left to
    // happen lazily that purge would land inside whichever test touched the
    // cache first, writing a format stamp into that one test's directory and no
    // other -- a difference between tests for no reason anybody could see.
    QTemporaryDir cacheBootstrap;
    if (cacheBootstrap.isValid()) {
        ThumbnailCache::setCacheDirectoryForTest(
            QDir(cacheBootstrap.path()).filePath(QStringLiteral("thumbnails")));
    }
    ThumbnailCache::instance();

    ::testing::InitGoogleTest(&argc, argv);
    ::testing::UnitTest::GetInstance()->listeners().Append(new ThumbnailCacheIsolation);
    return RUN_ALL_TESTS();
}
