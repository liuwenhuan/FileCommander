#include <gtest/gtest.h>

#include "CrashReporter.h"

#include "IconCache.h"
#include "ThemedDialogs.h"
#include "ImagePreviewLoader.h"
#include "ThumbnailCache.h"
#include "theme/Phosphor.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QSemaphore>
#include <QFont>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>

#include <iostream>
#include <memory>
#include <cstdlib>


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


// Fails the run when a test leaves the process-wide theme state changed.
//
// ThemeManager::apply() is not a widget operation: it installs an APPLICATION
// stylesheet, an application font, and the tints that recolour icons,
// thumbnails and preview images. MainWindow's constructor applies one too, from
// whatever settings it was given. The QApplication outlives every test here, so
// any of that left behind is an edit to the environment of everything that runs
// afterwards.
//
// This is not hypothetical. Two tests failed on every full-suite run and passed
// alone, for weeks, and both were this:
//
//   * a leaked stylesheet made a view stop propagating a font change to its
//     viewport (QStyleSheetStyle assigns a font at polish time), so a test
//     counting those events saw one instead of two;
//   * a leaked preview tint repainted a later test's images, turning a pure
//     green PNG into (105, 129, 161) and failing an assertion about which
//     image had won a race.
//
// Neither symptom pointed anywhere near the cause. The point of this sentinel
// is to name the offender AT the moment it offends, rather than let some
// unrelated test fail strangely half a suite later.
//
// It also PUTS THE STATE BACK, so one offender cannot cascade: without that,
// the first leak would make every later test look guilty too.
class ThemeStateSentinel : public ::testing::EmptyTestEventListener {
public:
    struct State {
        QString sheet;
        QFont font;
        QColor thumbnailTint;
        QColor previewTint;
        QColor glyphTint;
        QColor fileIconTint;
        // Compared by cache key: two QIcons are the same artwork only if they
        // came from the same pixmaps, and QIcon has no operator==.
        qint64 themedAppIcon = 0;

        static State capture() {
            return {qApp->styleSheet(),
                    qApp->font(),
                    fc::thumbnailTint(),
                    fc::previewTint(),
                    IconCache::instance().glyphTint(),
                    IconCache::instance().fileIconTint(),
                    ttc::themedAppIcon().cacheKey()};
        }
        void restore() const {
            // The themed mark is deliberately NOT restored here: it is a QIcon
            // and only its key was kept. Naming the offender is what this is
            // for; ThemeStateGuard is what puts it back.
            qApp->setStyleSheet(sheet);
            qApp->setFont(font);
            fc::setThumbnailTint(thumbnailTint);
            fc::setPreviewTint(previewTint);
            IconCache::instance().setGlyphTint(glyphTint);
            IconCache::instance().setFileIconTint(fileIconTint);
        }
    };

    void OnTestStart(const ::testing::TestInfo &) override { m_before = State::capture(); }

    void OnTestEnd(const ::testing::TestInfo &info) override {
        const State after = State::capture();
        QStringList changed;
        // The stylesheet is compared by LENGTH as well as content so the
        // message says something useful about a 16 kB theme sheet.
        if (after.sheet != m_before.sheet) {
            changed << QStringLiteral("stylesheet (%1 -> %2 chars)")
                           .arg(m_before.sheet.size())
                           .arg(after.sheet.size());
        }
        if (after.font != m_before.font) {
            changed << QStringLiteral("application font (%1 %2pt -> %3 %4pt)")
                           .arg(m_before.font.family())
                           .arg(m_before.font.pointSize())
                           .arg(after.font.family())
                           .arg(after.font.pointSize());
        }
        const auto tint = [&changed](const char *name, const QColor &before, const QColor &after) {
            if (before == after)
                return;
            changed << QStringLiteral("%1 (%2 -> %3)")
                           .arg(QString::fromLatin1(name),
                                before.isValid() ? before.name() : QStringLiteral("none"),
                                after.isValid() ? after.name() : QStringLiteral("none"));
        };
        tint("thumbnail tint", m_before.thumbnailTint, after.thumbnailTint);
        tint("preview tint", m_before.previewTint, after.previewTint);
        tint("glyph tint", m_before.glyphTint, after.glyphTint);
        tint("file icon tint", m_before.fileIconTint, after.fileIconTint);
        if (after.themedAppIcon != m_before.themedAppIcon)
            changed << QStringLiteral("themed app icon");

        if (changed.isEmpty())
            return;

        const QString who =
            QStringLiteral("%1.%2").arg(QString::fromLatin1(info.test_suite_name()),
                                        QString::fromLatin1(info.name()));
        std::cerr << "[  theme   ] " << qPrintable(who) << " left the process-wide theme state "
                  << "changed: " << qPrintable(changed.join(QStringLiteral("; "))) << std::endl
                  << "[  theme   ] declare a ThemeStateGuard (tests/ui/ThemeStateGuard.h) in it."
                  << std::endl;
        m_offenders << who;
        m_before.restore();
    }

    const QStringList &offenders() const { return m_offenders; }

private:
    State m_before;
    QStringList m_offenders;
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

    fc::installCrashReporter();

    ::testing::InitGoogleTest(&argc, argv);
    ::testing::UnitTest::GetInstance()->listeners().Append(new ThumbnailCacheIsolation);
    auto *themeState = new ThemeStateSentinel;
    ::testing::UnitTest::GetInstance()->listeners().Append(themeState);

    const int result = RUN_ALL_TESTS();
    int finalResult = result;
    if (!themeState->offenders().isEmpty()) {
        // A listener cannot record a gtest failure -- it runs outside any test body
        // -- so the verdict is delivered here, where main() owns the exit code.
        std::cerr << std::endl
                  << themeState->offenders().size()
                  << " test(s) left the process-wide theme state changed:" << std::endl;
        for (const QString &who : themeState->offenders())
            std::cerr << "    " << qPrintable(who) << std::endl;
        finalResult = result == 0 ? 1 : result;
    }
#ifdef Q_OS_WIN
    // Qt's Windows text-services teardown can raise a CRT dialog after all tests
    // have reported their result; the test verdict is complete before that cleanup.
    std::_Exit(finalResult);
#else
    return finalResult;
#endif
}
