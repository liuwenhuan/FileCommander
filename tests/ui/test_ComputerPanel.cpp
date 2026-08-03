#include <gtest/gtest.h>

#include <QBoxLayout>
#include <QDir>
#include <QLabel>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QWidget>

#include "BreadcrumbBar.h"
#include "FilePanel.h"
#include "FileListView.h"
#include "filesystem/ComputerProvider.h"

namespace {

ComputerEntry makeEntry(ComputerEntry::Kind kind, const QString &name, const QString &target) {
    ComputerEntry entry;
    entry.kind = kind;
    entry.name = name;
    entry.target = target;
    return entry;
}

QWidget *addressRow(FilePanel &panel) {
    for (QWidget *child : panel.findChildren<QWidget *>(QStringLiteral("PanelAddressRow")))
        return child;
    return nullptr;
}

// Waits for the panel's model to finish a listing.
void settle(FilePanel &panel) {
    QSignalSpy spy(panel.model(), &FileSystemModel::loadFinished);
    if (spy.isEmpty())
        spy.wait(5000);
    qApp->processEvents();
}

void activateRow(FilePanel &panel, int row) {
    QMetaObject::invokeMethod(panel.view(), "activated",
                              Q_ARG(QModelIndex, panel.model()->index(row, 0)));
}

} // namespace

TEST(ComputerPanelTest, ComputerButtonSitsBetweenTheForwardArrowAndThePath) {
    FilePanel panel;
    panel.resize(800, 500);
    panel.show();
    qApp->processEvents();

    QWidget *row = addressRow(panel);
    ASSERT_NE(row, nullptr);
    auto *layout = qobject_cast<QBoxLayout *>(row->layout());
    ASSERT_NE(layout, nullptr);

    auto *button = row->findChild<QToolButton *>(QStringLiteral("PanelComputerButton"));
    ASSERT_NE(button, nullptr);
    auto *breadcrumb = row->findChild<BreadcrumbBar *>();
    ASSERT_NE(breadcrumb, nullptr);

    const int buttonIndex = layout->indexOf(button);
    const int breadcrumbIndex = layout->indexOf(breadcrumb);
    ASSERT_GE(buttonIndex, 0);
    ASSERT_GE(breadcrumbIndex, 0);
    // Directly before the path, i.e. after the forward arrow: it is the level
    // above every path the breadcrumb can show.
    EXPECT_EQ(buttonIndex + 1, breadcrumbIndex);
    EXPECT_FALSE(button->icon().isNull());
}

TEST(ComputerPanelTest, PressingTheButtonAsksTheOwnerRatherThanListingOnItsOwn) {
    FilePanel panel;
    panel.resize(800, 500);
    panel.show();
    qApp->processEvents();

    QSignalSpy requested(&panel, &FilePanel::computerViewRequested);
    auto *button = panel.findChild<QToolButton *>(QStringLiteral("PanelComputerButton"));
    ASSERT_NE(button, nullptr);
    QTest::mouseClick(button, Qt::LeftButton);

    // The panel cannot see the device monitor or the host browser, so it must
    // not try to build the listing itself.
    ASSERT_EQ(requested.size(), 1);
    EXPECT_FALSE(panel.isComputerView());
}

TEST(ComputerPanelTest, ShowComputerListsThePlacesAndCaptionsTheAddressBar) {
    FilePanel panel;
    panel.resize(800, 500);
    panel.show();
    qApp->processEvents();

    panel.showComputer({
        makeEntry(ComputerEntry::Kind::Drive, QStringLiteral("Windows (C:)"), QStringLiteral("C:/")),
        makeEntry(ComputerEntry::Kind::SavedServer, QStringLiteral("Home NAS"),
                  QStringLiteral("uuid-1")),
    });
    settle(panel);

    EXPECT_TRUE(panel.isComputerView());
    ASSERT_EQ(panel.model()->rowCount(), 2);
    EXPECT_EQ(panel.model()->fileInfoAt(0).name(), QStringLiteral("Windows (C:)"));

    // "computer://" must never be rendered as a clickable path: split on "/" it
    // becomes segments that navigate nowhere.
    auto *breadcrumb = panel.findChild<BreadcrumbBar *>();
    ASSERT_NE(breadcrumb, nullptr);
    auto *label = breadcrumb->findChild<QLabel *>();
    ASSERT_NE(label, nullptr);
    EXPECT_FALSE(label->text().contains(QStringLiteral("computer:")));
    EXPECT_FALSE(label->text().contains(QStringLiteral("<a href")));
}

TEST(ComputerPanelTest, ShowComputerAgainRefreshesInPlaceInsteadOfReEntering) {
    FilePanel panel;
    panel.resize(800, 500);
    panel.show();
    qApp->processEvents();

    panel.showComputer(
        {makeEntry(ComputerEntry::Kind::Drive, QStringLiteral("C"), QStringLiteral("C:/"))});
    settle(panel);
    ASSERT_EQ(panel.model()->rowCount(), 1);

    // A device being plugged in re-pushes the whole list. Re-entering would
    // stack a history entry for a place the user never left, and would park the
    // synthetic provider as if it were the tab's real backend.
    panel.showComputer({
        makeEntry(ComputerEntry::Kind::Drive, QStringLiteral("C"), QStringLiteral("C:/")),
        makeEntry(ComputerEntry::Kind::RemovableDevice, QStringLiteral("USB"),
                  QStringLiteral("dev-1")),
    });
    settle(panel);

    EXPECT_TRUE(panel.isComputerView());
    EXPECT_EQ(panel.model()->rowCount(), 2);
}

TEST(ComputerPanelTest, ActivatingARowReportsItInsteadOfNavigatingToASyntheticPath) {
    FilePanel panel;
    panel.resize(800, 500);
    panel.show();
    qApp->processEvents();

    panel.showComputer({makeEntry(ComputerEntry::Kind::SavedServer, QStringLiteral("Home NAS"),
                                  QStringLiteral("uuid-1"))});
    settle(panel);
    ASSERT_EQ(panel.model()->rowCount(), 1);

    QSignalSpy activated(&panel, &FilePanel::computerEntryActivated);
    activateRow(panel, 0);
    qApp->processEvents();

    ASSERT_EQ(activated.size(), 1);
    const auto entry = activated.at(0).at(1).value<ComputerEntry>();
    EXPECT_EQ(entry.kind, ComputerEntry::Kind::SavedServer);
    EXPECT_EQ(entry.target, QStringLiteral("uuid-1"));
    // Still where it was: navigating to "computer://server/uuid-1" would have
    // asked the model to list a path that stands for a connection, not a place
    // with contents.
    EXPECT_EQ(panel.model()->rootPath(), ComputerProvider::rootPath());
}

TEST(ComputerPanelTest, LeavingTheViewPutsARealBackendBack) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir(dir.path()).mkdir(QStringLiteral("sub")));

    FilePanel panel;
    panel.resize(800, 500);
    panel.show();
    panel.navigateTo(dir.path());
    settle(panel);

    panel.showComputer(
        {makeEntry(ComputerEntry::Kind::Drive, QStringLiteral("C"), QStringLiteral("C:/"))});
    settle(panel);
    ASSERT_TRUE(panel.isComputerView());

    panel.leaveComputerView();
    panel.navigateTo(dir.path());
    settle(panel);

    EXPECT_FALSE(panel.isComputerView());
    EXPECT_EQ(QDir(panel.model()->rootPath()).canonicalPath(), QDir(dir.path()).canonicalPath());
    // The real listing is back -- and so is a clickable path.
    auto *breadcrumb = panel.findChild<BreadcrumbBar *>();
    ASSERT_NE(breadcrumb, nullptr);
    auto *label = breadcrumb->findChild<QLabel *>();
    ASSERT_NE(label, nullptr);
    EXPECT_TRUE(label->text().contains(QStringLiteral("<a href")));
}

TEST(ComputerPanelTest, TheSessionSnapshotRecordsARealPathNotTheSyntheticRoot) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    FilePanel panel;
    panel.resize(800, 500);
    panel.show();
    panel.navigateTo(dir.path());
    settle(panel);

    panel.showComputer(
        {makeEntry(ComputerEntry::Kind::Drive, QStringLiteral("C"), QStringLiteral("C:/"))});
    settle(panel);
    ASSERT_TRUE(panel.isComputerView());

    // This is what the shutdown path persists. "computer://" would come back on
    // the next launch as a tab the local provider cannot list, and the user
    // would have no way out of it except typing a path.
    const auto snapshot = panel.tabSnapshot();
    ASSERT_FALSE(snapshot.isEmpty());
    EXPECT_FALSE(snapshot.at(0).first.startsWith(ComputerProvider::rootPath()));
    EXPECT_EQ(QDir(snapshot.at(0).first).canonicalPath(), QDir(dir.path()).canonicalPath());
}

TEST(ComputerPanelTest, ATabOnTheComputerViewComesBackToItAfterASwitch) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir(dir.path()).mkdir(QStringLiteral("sub")));

    FilePanel panel;
    panel.resize(800, 500);
    panel.show();
    panel.navigateTo(dir.path());
    settle(panel);

    // Stand in for the owner, which is what holds the device monitor and host
    // browser and so is the only thing that can assemble the rows.
    int rebuilds = 0;
    QObject::connect(&panel, &FilePanel::computerViewRequested, &panel, [&](FilePanel *p) {
        ++rebuilds;
        p->showComputer({makeEntry(ComputerEntry::Kind::Drive, QStringLiteral("C"),
                                   QStringLiteral("C:/"))});
    });

    panel.showComputer(
        {makeEntry(ComputerEntry::Kind::Drive, QStringLiteral("C"), QStringLiteral("C:/"))});
    settle(panel);
    ASSERT_TRUE(panel.isComputerView());

    // Opening a second tab parks the first, exactly as switching tabs does.
    panel.newTab();
    settle(panel);
    EXPECT_FALSE(panel.isComputerView()) << "the new tab must start on a directory";

    panel.activateTab(0);
    settle(panel);

    // The tab was showing the computer view, so that is what it must come back
    // to -- not the directory underneath it, which is what a parked archive
    // browse does and what this used to do.
    EXPECT_TRUE(panel.isComputerView());
    EXPECT_EQ(panel.model()->rootPath(), ComputerProvider::rootPath());
    EXPECT_EQ(rebuilds, 1) << "the returning tab asks the owner to rebuild the rows";
}

TEST(ComputerPanelTest, ARestoredComputerTabStillLeavesToTheDirectoryItWasOpenedFrom) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir(dir.path()).mkdir(QStringLiteral("sub")));

    FilePanel panel;
    panel.resize(800, 500);
    panel.show();
    panel.navigateTo(dir.path());
    settle(panel);
    const int listedBefore = panel.model()->rowCount();
    ASSERT_GT(listedBefore, 0);

    QObject::connect(&panel, &FilePanel::computerViewRequested, &panel, [](FilePanel *p) {
        p->showComputer({makeEntry(ComputerEntry::Kind::Drive, QStringLiteral("C"),
                                   QStringLiteral("C:/"))});
    });
    panel.showComputer(
        {makeEntry(ComputerEntry::Kind::Drive, QStringLiteral("C"), QStringLiteral("C:/"))});
    settle(panel);
    panel.newTab();
    settle(panel);
    panel.activateTab(0);
    settle(panel);
    ASSERT_TRUE(panel.isComputerView());

    // The exit directory has to survive the round trip too, or leaving the
    // restored tab would land somewhere else -- or nowhere.
    panel.navigateTo(dir.path());
    settle(panel);
    EXPECT_FALSE(panel.isComputerView());
    EXPECT_EQ(QDir(panel.model()->rootPath()).canonicalPath(), QDir(dir.path()).canonicalPath());
    EXPECT_EQ(panel.model()->rowCount(), listedBefore);

    // And once left deliberately, a later switch must NOT drag it back.
    panel.newTab();
    settle(panel);
    panel.activateTab(0);
    settle(panel);
    EXPECT_FALSE(panel.isComputerView())
        << "a tab the user navigated away from was pulled back into the computer view";
}

TEST(ComputerPanelTest, NavigatingToADirectoryFromTheViewListsIt) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir(dir.path()).mkdir(QStringLiteral("sub")));

    FilePanel panel;
    panel.resize(800, 500);
    panel.show();
    panel.navigateTo(dir.path());
    settle(panel);
    const int listedBefore = panel.model()->rowCount();
    ASSERT_GT(listedBefore, 0);

    panel.showComputer(
        {makeEntry(ComputerEntry::Kind::Drive, QStringLiteral("C"), QStringLiteral("C:/"))});
    settle(panel);
    ASSERT_TRUE(panel.isComputerView());

    // Every other way of moving -- the folder tree, a favourite, Back, "go to
    // the other panel's directory" -- ends up here, at navigateTo() with an
    // ordinary path. The synthetic backend cannot list one: it answers empty for
    // anything that is not its own root, so the pane went blank.
    panel.navigateTo(dir.path());
    settle(panel);

    EXPECT_FALSE(panel.isComputerView());
    EXPECT_EQ(QDir(panel.model()->rootPath()).canonicalPath(), QDir(dir.path()).canonicalPath());
    EXPECT_EQ(panel.model()->rowCount(), listedBefore)
        << "navigating out of the computer view produced an empty listing";
}

TEST(ComputerPanelTest, BackFromTheComputerViewReturnsToWhereItWasOpenedFrom) {
    QTemporaryDir first;
    QTemporaryDir second;
    ASSERT_TRUE(first.isValid());
    ASSERT_TRUE(second.isValid());
    ASSERT_TRUE(QDir(second.path()).mkdir(QStringLiteral("sub")));

    FilePanel panel;
    panel.resize(800, 500);
    panel.show();
    panel.navigateTo(first.path());
    settle(panel);

    panel.showComputer(
        {makeEntry(ComputerEntry::Kind::Drive, QStringLiteral("C"), QStringLiteral("C:/"))});
    settle(panel);
    panel.navigateTo(second.path());
    settle(panel);
    ASSERT_EQ(QDir(panel.model()->rootPath()).canonicalPath(),
              QDir(second.path()).canonicalPath());

    // One Back, not two. The synthetic root must never reach the history stack:
    // nothing can restore it, so a Back that landed there would strand the panel
    // on a path no backend lists.
    panel.goBack();
    settle(panel);
    EXPECT_EQ(QDir(panel.model()->rootPath()).canonicalPath(),
              QDir(first.path()).canonicalPath());
    EXPECT_FALSE(panel.model()->rootPath().startsWith(ComputerProvider::rootPath()));
}

TEST(ComputerPanelTest, TypingAPathInTheAddressBarLeavesTheView) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    FilePanel panel;
    panel.resize(800, 500);
    panel.show();
    qApp->processEvents();
    panel.showComputer(
        {makeEntry(ComputerEntry::Kind::Drive, QStringLiteral("C"), QStringLiteral("C:/"))});
    settle(panel);
    ASSERT_TRUE(panel.isComputerView());

    auto *breadcrumb = panel.findChild<BreadcrumbBar *>();
    ASSERT_NE(breadcrumb, nullptr);
    // Without leaving first, the typed path would be resolved against the
    // synthetic backend, which does not know it, and the navigation would be
    // silently dropped.
    emit breadcrumb->pathActivated(dir.path());
    settle(panel);

    EXPECT_FALSE(panel.isComputerView());
    EXPECT_EQ(QDir(panel.model()->rootPath()).canonicalPath(), QDir(dir.path()).canonicalPath());
}
