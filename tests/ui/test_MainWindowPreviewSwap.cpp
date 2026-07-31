#include <gtest/gtest.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QImage>
#include <QItemSelectionModel>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSignalSpy>
#include <QShortcut>
#include <QSplitter>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBrowser>
#include <QTimer>
#include <QTreeView>

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
#include "tree/DirectoryTreeModel.h"

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

class StartupThemeObserver final : public QObject {
public:
    bool firstPaintSeen = false;
    int styleChangesAfterShow = 0;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        Q_UNUSED(watched);
        if (event->type() == QEvent::Paint)
            firstPaintSeen = true;
        else if (event->type() == QEvent::StyleChange)
            ++styleChangesAfterShow;
        return false;
    }
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

QRect scrollbarSliderRect(QScrollBar &scrollbar) {
    QStyleOptionSlider option;
    option.initFrom(&scrollbar);
    option.orientation = scrollbar.orientation();
    option.minimum = scrollbar.minimum();
    option.maximum = scrollbar.maximum();
    option.sliderPosition = scrollbar.sliderPosition();
    option.sliderValue = scrollbar.value();
    option.pageStep = scrollbar.pageStep();
    option.singleStep = scrollbar.singleStep();
    return scrollbar.style()->subControlRect(QStyle::CC_ScrollBar, &option,
                                             QStyle::SC_ScrollBarSlider, &scrollbar);
}

struct StartupThemeCase {
    Settings::Theme theme;
    const char *name;
    QColor handleColor;
};

class ScopedStartupThemeRestore final {
public:
    explicit ScopedStartupThemeRestore(Settings &settings)
        : settings(settings), theme(settings.theme()), styleSheet(qApp->styleSheet()) {}

    ~ScopedStartupThemeRestore() {
        settings.setTheme(theme);
        qApp->setStyleSheet(styleSheet);
        processGuiEvents();
    }

private:
    Settings &settings;
    Settings::Theme theme;
    QString styleSheet;
};

void populateStartupDirectory(const QTemporaryDir &directory, const QString &prefix) {
    for (int index = 0; index < 96; ++index) {
        QFile file(directory.filePath(QStringLiteral("%1-%2.txt").arg(prefix).arg(index, 3, 10,
                                                                                  QLatin1Char('0'))));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write("x"), 1);
    }
}

void expectThemedScrollbar(FileListView &view, const StartupThemeCase &themeCase) {
    ASSERT_TRUE(view.isVisible());
    QScrollBar *scrollbar = view.verticalScrollBar();
    ASSERT_NE(scrollbar, nullptr);
    ASSERT_TRUE(scrollbar->isVisible()) << themeCase.name;
    ASSERT_GT(scrollbar->maximum(), scrollbar->minimum()) << themeCase.name;

    const QRect slider = scrollbarSliderRect(*scrollbar);
    ASSERT_FALSE(slider.isEmpty()) << themeCase.name;

    const QImage rendered = scrollbar->grab().toImage();
    const QPoint handlePoint = slider.center();
    ASSERT_TRUE(rendered.rect().contains(handlePoint)) << themeCase.name;
    const QColor pixel = rendered.pixelColor(handlePoint);
    EXPECT_LE(qAbs(pixel.red() - themeCase.handleColor.red()) +
                  qAbs(pixel.green() - themeCase.handleColor.green()) +
                  qAbs(pixel.blue() - themeCase.handleColor.blue()),
              24)
        << themeCase.name << " scrollbar handle is not themed on the first paint";
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

class ScopedTypographySettingsRestore final {
public:
    explicit ScopedTypographySettingsRestore(Settings &settings)
        : settings(settings), family(settings.globalFontFamily()), size(settings.listFontSize()) {}

    ~ScopedTypographySettingsRestore() {
        settings.setGlobalFontFamily(family);
        settings.setListFontSize(size);
    }

private:
    Settings &settings;
    QString family;
    int size;
};

class ScopedFolderTreeSettingRestore final {
public:
    explicit ScopedFolderTreeSettingRestore(Settings &settings)
        : settings(settings), showFolderTree(settings.showFolderTree()) {}

    ~ScopedFolderTreeSettingRestore() {
        settings.setShowFolderTree(showFolderTree);
    }

private:
    Settings &settings;
    bool showFolderTree;
};

QTreeView *directoryTree(FilePanel &panel) {
    for (QTreeView *tree : panel.findChildren<QTreeView *>()) {
        if (qobject_cast<DirectoryTreeModel *>(tree->model()))
            return tree;
    }
    return nullptr;
}

} // namespace

class MainWindowStartupThemeTest : public ::testing::TestWithParam<StartupThemeCase> {};

TEST_P(MainWindowStartupThemeTest, FirstShowPaintsBothScrollbarsWithoutASecondGlobalStyleChange) {
    if (qApp->platformName() != QStringLiteral("windows"))
        GTEST_SKIP() << "requires the Windows QPA plugin";

    Settings settings;
    ScopedStartupThemeRestore restore(settings);
    settings.setTheme(GetParam().theme);
    qApp->setStyleSheet(QString());
    processGuiEvents();
    ASSERT_TRUE(qApp->styleSheet().isEmpty());

    QTemporaryDir leftDirectory;
    QTemporaryDir rightDirectory;
    ASSERT_TRUE(leftDirectory.isValid());
    ASSERT_TRUE(rightDirectory.isValid());
    populateStartupDirectory(leftDirectory, QStringLiteral("left"));
    populateStartupDirectory(rightDirectory, QStringLiteral("right"));

    MainWindow window;
    QSplitter *splitter = panelSplitter(window);
    ASSERT_NE(splitter, nullptr);
    auto *left = qobject_cast<FilePanel *>(splitter->widget(0));
    auto *right = qobject_cast<FilePanel *>(splitter->widget(1));
    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);

    left->navigateTo(leftDirectory.path());
    right->navigateTo(rightDirectory.path());
    QTRY_VERIFY_WITH_TIMEOUT(left->model()->rowCount() >= 96, 4000);
    QTRY_VERIFY_WITH_TIMEOUT(right->model()->rowCount() >= 96, 4000);

    StartupThemeObserver observer;
    window.installEventFilter(&observer);
    window.resize(1000, 700);
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(observer.firstPaintSeen, 1000);

    expectThemedScrollbar(*left->view(), GetParam());
    expectThemedScrollbar(*right->view(), GetParam());

    processGuiEvents();
    processGuiEvents();
    EXPECT_EQ(observer.styleChangesAfterShow, 0)
        << "startup must not reapply the application stylesheet after show begins";

    QFile expected(QStringLiteral(TTC_SOURCE_DIR "/resources/themes/") +
                   QString::fromLatin1(GetParam().name) + QStringLiteral(".qss"));
    ASSERT_TRUE(expected.open(QIODevice::ReadOnly | QIODevice::Text));
    EXPECT_EQ(qApp->styleSheet(), QString::fromUtf8(expected.readAll()));
}

INSTANTIATE_TEST_SUITE_P(
    WindowsQpa, MainWindowStartupThemeTest,
    ::testing::Values(StartupThemeCase{Settings::Theme::Dark, "dark", QColor(0x4a, 0x4a, 0x4a)},
                      StartupThemeCase{Settings::Theme::Light, "light", QColor(0xc0, 0xc0, 0xc0)},
                      StartupThemeCase{Settings::Theme::Crt, "green", QColor(0x12, 0x60, 0x2f)}),
    [](const ::testing::TestParamInfo<StartupThemeCase> &info) { return info.param.name; });

TEST(MainWindowPreviewSwapTest, AutomaticMediaWarmupStaysDisabledAfterFirstVisiblePaint) {
    QElapsedTimer startup;
    startup.start();
    MainWindow window;
    RecordProperty("main_window_construct_ms", std::to_string(startup.elapsed()));
    EXPECT_TRUE(window.findChildren<QuickView *>().isEmpty());

    QElapsedTimer firstPaint;
    PaintObserver paintObserver(firstPaint);
    window.installEventFilter(&paintObserver);
    firstPaint.start();
    window.resize(1000, 700);
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(paintObserver.firstPaintMs >= 0, 1000);
    RecordProperty("show_to_first_paint_ms",
                   std::to_string(paintObserver.firstPaintMs));

    EXPECT_TRUE(window.findChildren<QuickView *>().isEmpty());
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "toggleQuickView"));
    processGuiEvents();

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

TEST(MainWindowPreviewSwapTest, FirstQuickViewUseCreatesOneInstanceWithCurrentTypography) {
    Settings settings;
    ScopedTypographySettingsRestore restore(settings);
    settings.setGlobalFontFamily(QStringLiteral("Arial"));
    settings.setListFontSize(15);

    MainWindow window;

    EXPECT_TRUE(window.findChildren<QuickView *>().isEmpty());

    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "toggleQuickView"));
    processGuiEvents();

    const QList<QuickView *> firstQuickViews = window.findChildren<QuickView *>();
    ASSERT_EQ(firstQuickViews.size(), 1);
    QuickView *const quickView = firstQuickViews.constFirst();
    auto *markdown = quickView->findChild<QTextBrowser *>();
    ASSERT_NE(markdown, nullptr);
    EXPECT_EQ(markdown->font().family(), QStringLiteral("Arial"));
    EXPECT_EQ(markdown->font().pointSize(), 15);

    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "toggleQuickView"));
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "toggleQuickView"));
    processGuiEvents();

    const QList<QuickView *> secondQuickViews = window.findChildren<QuickView *>();
    ASSERT_EQ(secondQuickViews.size(), 1);
    EXPECT_EQ(secondQuickViews.constFirst(), quickView);
}

TEST(MainWindowPreviewSwapTest, TypographySettingsDoNotLeakPastTestScope) {
    Settings settings;
    const QString originalFamily = settings.globalFontFamily();
    const int originalSize = settings.listFontSize();
    const int changedSize = originalSize == 15 ? 14 : 15;

    {
        Settings changed;
        ScopedTypographySettingsRestore restore(changed);
        changed.setGlobalFontFamily(QStringLiteral("Task2QuickViewTypographySentinel"));
        changed.setListFontSize(changedSize);
    }

    Settings after;
    EXPECT_EQ(after.globalFontFamily(), originalFamily);
    EXPECT_EQ(after.listFontSize(), originalSize);
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
    for (const QString &path : {leftDirectory.filePath(QStringLiteral("left-a.txt")),
                                leftDirectory.filePath(QStringLiteral("left-b.txt")),
                                rightDirectory.filePath(QStringLiteral("right-a.txt")),
                                rightDirectory.filePath(QStringLiteral("right-b.txt"))}) {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    }

    MainWindow window(nullptr, 10000);
    QSplitter *splitter = panelSplitter(window);
    ASSERT_NE(splitter, nullptr);
    auto *left = qobject_cast<FilePanel *>(splitter->widget(0));
    auto *right = qobject_cast<FilePanel *>(splitter->widget(1));
    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);

    QSignalSpy ready(&window, &MainWindow::startupReady);
    QSignalSpy leftLoaded(left->model(), &FileSystemModel::loadFinished);
    QSignalSpy rightLoaded(right->model(), &FileSystemModel::loadFinished);
    QSignalSpy leftCurrentChanged(left->activeView()->selectionModel(),
                                  &QItemSelectionModel::currentChanged);
    QSignalSpy rightCurrentChanged(right->activeView()->selectionModel(),
                                   &QItemSelectionModel::currentChanged);
    bool readyAfterBothLoads = false;
    bool readyAfterSelectionProbes = false;
    QObject::connect(&window, &MainWindow::startupReady, [&] {
        readyAfterBothLoads = !leftLoaded.isEmpty() && !rightLoaded.isEmpty();
        readyAfterSelectionProbes = leftCurrentChanged.count() >= 2 &&
                                    rightCurrentChanged.count() >= 2 &&
                                    left->activeView()->currentIndex().row() == 1 &&
                                    right->activeView()->currentIndex().row() == 1;
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
    EXPECT_EQ(left->activeView()->currentIndex().row(), 1);
    EXPECT_EQ(right->activeView()->currentIndex().row(), 1);
    const QJsonObject metrics = window.startupMetrics();
    EXPECT_GE(metrics.value(QStringLiteral("visibleMs")).toInt(), 10000);
    EXPECT_GE(metrics.value(QStringLiteral("panelsLoadedMs")).toInt(),
              metrics.value(QStringLiteral("visibleMs")).toInt());
    EXPECT_GE(metrics.value(QStringLiteral("interactiveMs")).toInt(),
              metrics.value(QStringLiteral("panelsLoadedMs")).toInt());
    processGuiEvents();
    EXPECT_EQ(ready.count(), 1);
}

TEST(MainWindowStartupTest, EmitsReadyWhenBothPanelsFinishLoadingBeforeShow) {
    QTemporaryDir leftDirectory;
    QTemporaryDir rightDirectory;
    ASSERT_TRUE(leftDirectory.isValid());
    ASSERT_TRUE(rightDirectory.isValid());
    const QString leftFirst = QStringLiteral("task5-left-before-show-a.txt");
    const QString leftSecond = QStringLiteral("task5-left-before-show-b.txt");
    const QString rightFirst = QStringLiteral("task5-right-before-show-a.txt");
    const QString rightSecond = QStringLiteral("task5-right-before-show-b.txt");
    for (const QString &path : {leftDirectory.filePath(leftFirst),
                                leftDirectory.filePath(leftSecond),
                                rightDirectory.filePath(rightFirst),
                                rightDirectory.filePath(rightSecond)}) {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    }

    MainWindow window;
    QSplitter *splitter = panelSplitter(window);
    ASSERT_NE(splitter, nullptr);
    auto *left = qobject_cast<FilePanel *>(splitter->widget(0));
    auto *right = qobject_cast<FilePanel *>(splitter->widget(1));
    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);

    QSignalSpy leftLoaded(left->model(), &FileSystemModel::loadFinished);
    QSignalSpy rightLoaded(right->model(), &FileSystemModel::loadFinished);
    left->navigateTo(leftDirectory.path());
    right->navigateTo(rightDirectory.path());
    QTRY_VERIFY_WITH_TIMEOUT(!leftLoaded.isEmpty(), 4000);
    QTRY_VERIFY_WITH_TIMEOUT(!rightLoaded.isEmpty(), 4000);
    const auto modelContains = [](FileSystemModel *model, const QString &name) {
        for (int row = 0; row < model->rowCount(); ++row) {
            if (model->data(model->index(row, 0), Qt::DisplayRole).toString() == name)
                return true;
        }
        return false;
    };
    QTRY_VERIFY_WITH_TIMEOUT(modelContains(left->model(), leftFirst) &&
                                 modelContains(left->model(), leftSecond),
                             4000);
    QTRY_VERIFY_WITH_TIMEOUT(modelContains(right->model(), rightFirst) &&
                                 modelContains(right->model(), rightSecond),
                             4000);
    ASSERT_FALSE(window.isVisible());
    ASSERT_GE(left->model()->rowCount(), 2);
    ASSERT_GE(right->model()->rowCount(), 2);
    const QModelIndex leftBeforeShow = left->activeView()->currentIndex();
    const QModelIndex rightBeforeShow = right->activeView()->currentIndex();
    ASSERT_TRUE(leftBeforeShow.isValid());
    ASSERT_TRUE(rightBeforeShow.isValid());
    const int expectedLeftRow = leftBeforeShow.row() == 0 ? 1 : 0;
    const int expectedRightRow = rightBeforeShow.row() == 0 ? 1 : 0;
    processGuiEvents();
    processGuiEvents();
    ASSERT_FALSE(window.isVisible());
    ASSERT_EQ(window.startupMetrics().value(QStringLiteral("visibleMs")).toInt(), -1);
    ASSERT_GE(window.startupMetrics().value(QStringLiteral("panelsLoadedMs")).toInt(), 0);
    std::vector<std::unique_ptr<QSignalBlocker>> modelBlockers;
    for (FileSystemModel *model : window.findChildren<FileSystemModel *>())
        modelBlockers.push_back(std::make_unique<QSignalBlocker>(model));
    ASSERT_GE(modelBlockers.size(), 2);

    QSignalSpy ready(&window, &MainWindow::startupReady);
    window.resize(1000, 700);
    window.show();

    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 1000);
    EXPECT_EQ(left->activeView()->currentIndex().row(), expectedLeftRow);
    EXPECT_EQ(right->activeView()->currentIndex().row(), expectedRightRow);
    EXPECT_GE(window.startupMetrics().value(QStringLiteral("panelsLoadedMs")).toInt(),
              window.startupMetrics().value(QStringLiteral("visibleMs")).toInt());
    processGuiEvents();
    EXPECT_EQ(ready.count(), 1);
}

TEST(MainWindowStartupTest, RestoresFolderTreesBeforeReadinessWhenEnabled) {
    Settings settings;
    ScopedFolderTreeSettingRestore restore(settings);
    settings.setShowFolderTree(true);

    QTemporaryDir leftDirectory;
    QTemporaryDir rightDirectory;
    ASSERT_TRUE(leftDirectory.isValid());
    ASSERT_TRUE(rightDirectory.isValid());
    for (const QString &path : {leftDirectory.filePath(QStringLiteral("left.txt")),
                                rightDirectory.filePath(QStringLiteral("right.txt"))}) {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    }

    MainWindow window(nullptr, 10000);
    QSplitter *splitter = panelSplitter(window);
    ASSERT_NE(splitter, nullptr);
    auto *left = qobject_cast<FilePanel *>(splitter->widget(0));
    auto *right = qobject_cast<FilePanel *>(splitter->widget(1));
    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);
    EXPECT_NE(directoryTree(*left), nullptr);
    EXPECT_NE(directoryTree(*right), nullptr);

    QSignalSpy ready(&window, &MainWindow::startupReady);
    QSignalSpy leftLoaded(left->model(), &FileSystemModel::loadFinished);
    QSignalSpy rightLoaded(right->model(), &FileSystemModel::loadFinished);
    bool readyAfterBothLoads = false;
    QObject::connect(&window, &MainWindow::startupReady, [&] {
        readyAfterBothLoads = !leftLoaded.isEmpty() && !rightLoaded.isEmpty();
    });

    left->navigateTo(leftDirectory.path());
    right->navigateTo(rightDirectory.path());
    window.resize(1000, 700);
    window.show();

    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 4000);
    EXPECT_TRUE(readyAfterBothLoads);
    processGuiEvents();
    EXPECT_EQ(ready.count(), 1);
}

TEST(MainWindowStartupTest, KeepsFolderTreesLazyWhenRestoredSettingIsDisabled) {
    Settings settings;
    ScopedFolderTreeSettingRestore restore(settings);
    settings.setShowFolderTree(false);

    MainWindow window;
    QSplitter *splitter = panelSplitter(window);
    ASSERT_NE(splitter, nullptr);
    auto *left = qobject_cast<FilePanel *>(splitter->widget(0));
    auto *right = qobject_cast<FilePanel *>(splitter->widget(1));
    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);
    EXPECT_EQ(directoryTree(*left), nullptr);
    EXPECT_EQ(directoryTree(*right), nullptr);
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
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "toggleQuickView"));
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
    EXPECT_TRUE(window->findChildren<QuickView *>().isEmpty());

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
