#include <gtest/gtest.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QSignalSpy>
#include <QShortcut>
#include <QSplitter>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <clocale>
#include <memory>
#include <string>

#include "BreadcrumbBar.h"
#include "FileProvider.h"
#include "FilePanel.h"
#include "FileSystemModel.h"
#include "MainWindow.h"
#include "QuickView.h"
#include "TabBar.h"
#include "config/Settings.h"
#include "devices/RemovableDeviceMonitor.h"
#include "media/MediaEngine.h"
#include "network/SmbHostBrowser.h"

namespace {

class PaintObserver final : public QObject {
public:
    explicit PaintObserver(QElapsedTimer &elapsed) : elapsed(elapsed) {}

    qint64 firstPaintMs = -1;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() == QEvent::Paint && firstPaintMs < 0)
            firstPaintMs = elapsed.elapsed();
        return QObject::eventFilter(watched, event);
    }

private:
    QElapsedTimer &elapsed;
};

QSplitter *panelSplitter(MainWindow &window) {
    for (QSplitter *splitter : window.findChildren<QSplitter *>()) {
        if (splitter->orientation() != Qt::Horizontal || splitter->count() != 2)
            continue;
        if (qobject_cast<FilePanel *>(splitter->widget(0)) ||
            qobject_cast<FilePanel *>(splitter->widget(1)) ||
            qobject_cast<QuickView *>(splitter->widget(0)) ||
            qobject_cast<QuickView *>(splitter->widget(1)))
            return splitter;
    }
    return nullptr;
}

void processGuiEvents() {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 1000);
}

class FakeSwapShare : public FileProvider {
public:
    QString displayName() const override { return QStringLiteral("tester@swap-share"); }
    QString scheme() const override { return QStringLiteral("smb"); }

    QVector<FileInfo> list(const QString &path, bool) const override {
        if (cleanPath(path) != QStringLiteral("/share/docs"))
            return {};
        return {FileInfo::fromFields(QStringLiteral("/share/docs/remote.txt"),
                                     QStringLiteral("remote.txt"), 11,
                                     QDateTime::fromSecsSinceEpoch(1000000), false,
                                     QFile::ReadOwner)};
    }
    bool isDir(const QString &path) const override {
        return cleanPath(path) == QStringLiteral("/share/docs");
    }
    QString cleanPath(const QString &path) const override {
        QString clean = path;
        while (clean.size() > 1 && clean.endsWith(QLatin1Char('/')))
            clean.chop(1);
        return clean;
    }
    QString parentPath(const QString &path) const override {
        const QString clean = cleanPath(path);
        const int slash = clean.lastIndexOf(QLatin1Char('/'));
        if (slash < 0)
            return {};
        return slash == 0 ? QStringLiteral("/") : clean.left(slash);
    }
    bool exists(const QString &path) const override {
        const QString clean = cleanPath(path);
        return clean == QStringLiteral("/share/docs") ||
               clean == QStringLiteral("/share/docs/remote.txt");
    }
    RenameResult rename(const QString &, const QString &, QString *) override {
        return RenameResult::Failed;
    }
};

void settle(FilePanel &panel) {
    QSignalSpy spy(panel.model(), &FileSystemModel::loadFinished);
    if (spy.isEmpty())
        spy.wait(4000);
    processGuiEvents();
}

} // namespace

TEST(MainWindowPreviewSwapTest, AutomaticMediaWarmupStaysDisabledAfterFirstVisiblePaint) {
    QElapsedTimer startup;
    startup.start();
    MainWindow window;
    RecordProperty("main_window_construct_ms", std::to_string(startup.elapsed()));
    auto *quickView = window.findChild<QuickView *>();
    auto *warmTimer = window.findChild<QTimer *>(QStringLiteral("mediaWarmTimer"));
    ASSERT_NE(quickView, nullptr);
    ASSERT_NE(warmTimer, nullptr);
    QSignalSpy warmed(quickView, &QuickView::mediaEngineWarmed);
    EXPECT_FALSE(warmTimer->isActive());
    EXPECT_TRUE(warmTimer->isSingleShot());
    EXPECT_EQ(warmTimer->interval(), 750);
#if FILECOMMANDER_HAS_PREVIEW_MEDIA
    EXPECT_TRUE(quickView->findChildren<MediaEngine *>().isEmpty());
#else
    EXPECT_EQ(quickView->findChildren<MediaEngine *>().size(), 1);
#endif
    EXPECT_EQ(quickView->findChild<QWidget *>(QStringLiteral("quickViewVideoPage")), nullptr);
    EXPECT_EQ(quickView->findChild<QWidget *>(QStringLiteral("quickViewAudioPage")), nullptr);

    QElapsedTimer firstPaint;
    PaintObserver paintObserver(firstPaint);
    window.installEventFilter(&paintObserver);
    firstPaint.start();
    window.resize(1000, 700);
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(paintObserver.firstPaintMs >= 0, 1000);
    RecordProperty("show_to_first_paint_ms",
                   std::to_string(paintObserver.firstPaintMs));

    EXPECT_FALSE(warmTimer->isActive());
    EXPECT_EQ(quickView->findChild<QWidget *>(QStringLiteral("quickViewVideoPage")), nullptr);
    QTest::qWait(850);
    EXPECT_EQ(warmed.count(), 0);
    EXPECT_FALSE(warmTimer->isActive());
#if FILECOMMANDER_HAS_PREVIEW_MEDIA
    EXPECT_TRUE(quickView->findChildren<MediaEngine *>().isEmpty());
#else
    EXPECT_EQ(quickView->findChildren<MediaEngine *>().size(), 1);
#endif
    EXPECT_EQ(quickView->findChild<QWidget *>(QStringLiteral("quickViewVideoPage")), nullptr);
    EXPECT_EQ(quickView->findChild<QWidget *>(QStringLiteral("quickViewAudioPage")), nullptr);
    window.update();
    processGuiEvents();
    EXPECT_EQ(warmed.count(), 0);
}

TEST(MainWindowStartupTest, DefersBackgroundFeatureBatchPastFirstPaint) {
    MainWindow window;

#if FILECOMMANDER_HAS_LINUX_INTEGRATION || defined(Q_OS_WIN)
    EXPECT_EQ(window.findChild<RemovableDeviceMonitor *>(), nullptr);
#endif
#if FILECOMMANDER_HAS_LINUX_INTEGRATION || (defined(Q_OS_WIN) && FILECOMMANDER_HAS_NETWORK)
    EXPECT_EQ(window.findChild<SmbHostBrowser *>(), nullptr);
#endif

    window.resize(1000, 700);
    window.show();
    processGuiEvents();
    processGuiEvents();

#if FILECOMMANDER_HAS_LINUX_INTEGRATION || defined(Q_OS_WIN)
    EXPECT_EQ(window.findChild<RemovableDeviceMonitor *>(), nullptr);
#endif
#if FILECOMMANDER_HAS_LINUX_INTEGRATION || (defined(Q_OS_WIN) && FILECOMMANDER_HAS_NETWORK)
    EXPECT_EQ(window.findChild<SmbHostBrowser *>(), nullptr);
#endif
}

TEST(MainWindowStartupTest, EmitsReadyAfterBothVisiblePanelsAreInteractive) {
    QTemporaryDir leftDirectory;
    QTemporaryDir rightDirectory;
    ASSERT_TRUE(leftDirectory.isValid());
    ASSERT_TRUE(rightDirectory.isValid());

    MainWindow window;
    QSplitter *splitter = panelSplitter(window);
    ASSERT_NE(splitter, nullptr);
    auto *left = qobject_cast<FilePanel *>(splitter->widget(0));
    auto *right = qobject_cast<FilePanel *>(splitter->widget(1));
    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);

    QSignalSpy ready(&window, &MainWindow::startupReady);
    QSignalSpy leftLoaded(left->model(), &FileSystemModel::loadFinished);
    QSignalSpy rightLoaded(right->model(), &FileSystemModel::loadFinished);
    bool readyAfterBothLoads = false;
    bool readyAfterSelectionProbes = false;
    QObject::connect(&window, &MainWindow::startupReady, [&] {
        readyAfterBothLoads = !leftLoaded.isEmpty() && !rightLoaded.isEmpty();
        readyAfterSelectionProbes = left->activeView()->currentIndex().isValid() &&
                                   right->activeView()->currentIndex().isValid();
    });

    left->navigateTo(leftDirectory.path());
    right->navigateTo(rightDirectory.path());
    window.resize(1000, 700);
    window.show();

    QTRY_VERIFY_WITH_TIMEOUT(ready.count() == 1, 4000);
    EXPECT_GE(leftLoaded.count(), 1);
    EXPECT_GE(rightLoaded.count(), 1);
    EXPECT_TRUE(readyAfterBothLoads);
    EXPECT_TRUE(readyAfterSelectionProbes);
    EXPECT_TRUE(left->activeView()->currentIndex().isValid());
    EXPECT_TRUE(right->activeView()->currentIndex().isValid());
    processGuiEvents();
    EXPECT_EQ(ready.count(), 1);
}

TEST(MainWindowPreviewSwapTest, FirstMediaRequestWarmsImmediatelyWithAutomaticWarmupDisabled) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString audio = dir.filePath(QStringLiteral("requested-first.wav"));
    QFile file(audio);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("audio");
    file.close();

    MainWindow window;
    window.resize(1000, 700);
    window.show();
    processGuiEvents();
    auto *quickView = window.findChild<QuickView *>();
    auto *warmTimer = window.findChild<QTimer *>(QStringLiteral("mediaWarmTimer"));
    ASSERT_NE(quickView, nullptr);
    ASSERT_NE(warmTimer, nullptr);
    ASSERT_FALSE(warmTimer->isActive());

    quickView->showFile(audio);

    EXPECT_FALSE(warmTimer->isActive());
    EXPECT_NE(quickView->findChild<QWidget *>(QStringLiteral("quickViewAudioPage")), nullptr);
    EXPECT_EQ(quickView->findChild<QWidget *>(QStringLiteral("quickViewVideoPage")), nullptr);
    EXPECT_EQ(quickView->findChildren<MediaEngine *>().size(), 1);
    QTest::qWait(800);
    EXPECT_EQ(quickView->findChildren<MediaEngine *>().size(), 1);
}

TEST(MainWindowPreviewSwapTest, TeardownIsSafeWithAutomaticWarmupDisabled) {
    auto *window = new MainWindow;
    window->resize(1000, 700);
    window->show();
    processGuiEvents();
    auto *warmTimer = window->findChild<QTimer *>(QStringLiteral("mediaWarmTimer"));
    ASSERT_NE(warmTimer, nullptr);
    ASSERT_FALSE(warmTimer->isActive());

    delete window;
    QTest::qWait(800);

    SUCCEED();
}

TEST(MainWindowPreviewSwapTest, CtrlUSwapsPreviewWithVisiblePanelAndKeepsHiddenPanelParked) {
    std::setlocale(LC_NUMERIC, "C");
    Settings settings;
    const QKeySequence previousSwap =
        settings.shortcut(QStringLiteral("swapPanels"), QKeySequence(Qt::CTRL | Qt::Key_U));
    settings.setShortcut(QStringLiteral("swapPanels"), QKeySequence(Qt::CTRL | Qt::Key_U));
    struct ShortcutRestore {
        QKeySequence previous;
        ~ShortcutRestore() { Settings().setShortcut(QStringLiteral("swapPanels"), previous); }
    } restore{previousSwap};

    MainWindow window;
    window.resize(1000, 700);
    window.show();
    processGuiEvents();

    bool ctrlUBound = false;
    for (QShortcut *shortcut : window.findChildren<QShortcut *>()) {
        if (shortcut->key() == QKeySequence(Qt::CTRL | Qt::Key_U)) {
            ctrlUBound = true;
            break;
        }
    }
    ASSERT_TRUE(ctrlUBound) << "Ctrl+U must remain bound to the panel swap command";

    QSplitter *splitter = panelSplitter(window);
    ASSERT_NE(splitter, nullptr);
    auto *left = qobject_cast<FilePanel *>(splitter->widget(0));
    auto *right = qobject_cast<FilePanel *>(splitter->widget(1));
    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);

    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "toggleQuickView"));
    processGuiEvents();
    ASSERT_EQ(splitter->widget(0), left);
    ASSERT_NE(qobject_cast<QuickView *>(splitter->widget(1)), nullptr);
    EXPECT_EQ(right->parentWidget(), nullptr) << "inactive panel must be parked";

    left->view()->setFocus();
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "swapPanels"));
    processGuiEvents();

    EXPECT_NE(qobject_cast<QuickView *>(splitter->widget(0)), nullptr);
    EXPECT_EQ(splitter->widget(1), left);
    EXPECT_EQ(right->parentWidget(), nullptr) << "Ctrl+U must not reveal the hidden panel";
    EXPECT_EQ(window.focusWidget(), left->activeView())
        << "visible file panel remains the window's keyboard target after the swap";

    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "toggleQuickView"));
    processGuiEvents();

    EXPECT_EQ(splitter->widget(0), right) << "closing restores the parked panel to preview's new slot";
    EXPECT_EQ(splitter->widget(1), left) << "closing keeps the swapped panel position";
    EXPECT_EQ(window.focusWidget(), left->activeView());
}

TEST(MainWindowLayoutTest, LongDirectoryNamesDoNotSetThePanelMinimumWidth) {
    const QString longDirectory =
        QStringLiteral("this-is-a-deliberately-long-directory-name-that-must-not-lock-the-panel-width");

    QSplitter splitter(Qt::Horizontal);
    auto *left = new FilePanel(&splitter);
    auto *right = new FilePanel(&splitter);
    splitter.addWidget(left);
    splitter.addWidget(right);
    splitter.resize(1200, 700);
    splitter.show();
    processGuiEvents();

    TabBar *tabs = left->findChild<TabBar *>();
    BreadcrumbBar *address = left->findChild<BreadcrumbBar *>();
    ASSERT_NE(tabs, nullptr);
    ASSERT_NE(address, nullptr);
    tabs->setTabText(0, longDirectory);
    address->setPath(QStringLiteral("/tmp/") + longDirectory);
    processGuiEvents();

    splitter.setSizes({260, 900});
    processGuiEvents();
    EXPECT_LE(splitter.sizes().at(0), 300);
}

TEST(MainWindowPreviewSwapTest, SwapPanelsMovesRemoteConnectionWithItsPath) {
    QTemporaryDir localDir;
    ASSERT_TRUE(localDir.isValid());

    MainWindow window;
    window.resize(1000, 700);
    window.show();
    processGuiEvents();

    QSplitter *splitter = panelSplitter(window);
    ASSERT_NE(splitter, nullptr);
    auto *left = qobject_cast<FilePanel *>(splitter->widget(0));
    auto *right = qobject_cast<FilePanel *>(splitter->widget(1));
    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);

    auto share = std::make_shared<FakeSwapShare>();
    left->connectTabTo(0, share, [](QString *) { return true; },
                       QStringLiteral("/share/docs"), QStringLiteral("tester@swap-share"),
                       SavedConnection{}, FileSystemModel::AuthRetryFactory());
    settle(*left);
    right->navigateTo(localDir.path());
    settle(*right);
    ASSERT_TRUE(left->model()->hasNetworkSession());
    const QString connectionId = left->connectionId();

    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "swapPanels"));
    processGuiEvents();
    settle(*left);
    settle(*right);

    EXPECT_EQ(left->currentPath(), localDir.path());
    EXPECT_FALSE(left->model()->hasNetworkSession());
    EXPECT_EQ(right->currentPath(), QStringLiteral("/share/docs"));
    EXPECT_TRUE(right->model()->hasNetworkSession());
    EXPECT_EQ(right->connectionId(), connectionId);
}
