#include <gtest/gtest.h>

#include <QApplication>
#include <QSignalSpy>
#include <QShortcut>
#include <QSplitter>
#include <QTemporaryDir>

#include <clocale>
#include <memory>

#include "BreadcrumbBar.h"
#include "FileProvider.h"
#include "FilePanel.h"
#include "FileSystemModel.h"
#include "MainWindow.h"
#include "QuickView.h"
#include "TabBar.h"
#include "config/Settings.h"

namespace {

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
