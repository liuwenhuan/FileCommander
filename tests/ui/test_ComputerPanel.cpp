#include <gtest/gtest.h>

#include <QBoxLayout>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
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
#include "FileProvider.h"
#include "FileSystemModel.h"
#include "filesystem/ComputerProvider.h"

#include <memory>

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
    EXPECT_FALSE(snapshot.at(0).path.startsWith(ComputerProvider::rootPath()));
    EXPECT_EQ(QDir(snapshot.at(0).path).canonicalPath(), QDir(dir.path()).canonicalPath());
    // ... and the fact that it WAS the computer view travels beside it, so the
    // next launch can put the tab back rather than dropping it on the directory
    // underneath.
    EXPECT_TRUE(snapshot.at(0).computerView);
}

TEST(ComputerPanelTest, ASavedComputerTabComesBackAsTheComputerViewNextLaunch) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    // First run: a tab on the computer view, snapshotted the way shutdown does.
    QVector<FilePanel::RestoredTab> saved;
    {
        FilePanel panel;
        panel.resize(800, 500);
        panel.show();
        panel.navigateTo(dir.path());
        settle(panel);
        panel.showComputer({makeEntry(ComputerEntry::Kind::Drive, QStringLiteral("C"),
                                      QStringLiteral("C:/"))});
        settle(panel);
        ASSERT_TRUE(panel.isComputerView());
        saved = panel.tabSnapshot();
    }
    ASSERT_EQ(saved.size(), 1);
    ASSERT_TRUE(saved.at(0).computerView);

    // Next launch. restoreTabs runs before the owner connects its signal, which
    // is why the panel falls back to the directory first and the owner has to
    // ask afterwards -- reproducing the real startup order rather than a
    // convenient one.
    FilePanel next;
    next.resize(800, 500);
    next.show();
    next.restoreTabs(saved, 0);
    settle(next);
    EXPECT_FALSE(next.isComputerView()) << "nothing can build the rows yet";
    EXPECT_EQ(QDir(next.model()->rootPath()).canonicalPath(), QDir(dir.path()).canonicalPath());

    // The intent survived the fallback, so the owner can act on it once wired.
    ASSERT_TRUE(next.activeTabWantsComputerView());
    QObject::connect(&next, &FilePanel::computerViewRequested, &next, [](FilePanel *p) {
        p->showComputer({makeEntry(ComputerEntry::Kind::Drive, QStringLiteral("C"),
                                   QStringLiteral("C:/"))});
    });
    next.showComputer(
        {makeEntry(ComputerEntry::Kind::Drive, QStringLiteral("C"), QStringLiteral("C:/"))});
    settle(next);
    EXPECT_TRUE(next.isComputerView());
    EXPECT_EQ(next.model()->rootPath(), ComputerProvider::rootPath());
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

TEST(ComputerPanelTest, BackFromADriveOpenedInTheViewReturnsToTheView) {
    QTemporaryDir dir;
    QTemporaryDir opened;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(opened.isValid());
    ASSERT_TRUE(QDir(opened.path()).mkdir(QStringLiteral("sub")));

    FilePanel panel;
    panel.resize(800, 500);
    panel.show();
    panel.navigateTo(dir.path());
    settle(panel);

    QObject::connect(&panel, &FilePanel::computerViewRequested, &panel, [](FilePanel *p) {
        p->showComputer({makeEntry(ComputerEntry::Kind::Drive, QStringLiteral("C"),
                                   QStringLiteral("C:/"))});
    });
    panel.showComputer(
        {makeEntry(ComputerEntry::Kind::Drive, QStringLiteral("C"), QStringLiteral("C:/"))});
    settle(panel);
    ASSERT_TRUE(panel.isComputerView());

    // Open one of the places it listed -- the same thing activating a drive row
    // does once the target has been resolved.
    panel.navigateTo(opened.path());
    settle(panel);
    ASSERT_FALSE(panel.isComputerView());

    // Back has to land on the computer view, not skip past it to the directory
    // underneath. The view is rebuilt rather than restored, so what comes back
    // is a fresh listing.
    panel.goBack();
    settle(panel);
    EXPECT_TRUE(panel.isComputerView());
    EXPECT_EQ(panel.model()->rootPath(), ComputerProvider::rootPath());
}

namespace {

// A stand-in for a mounted server, enough for the panel to hold a session.
class FakeShare : public FileProvider {
public:
    explicit FakeShare(QString root) : m_root(std::move(root)) {}

    QString displayName() const override { return QStringLiteral("tester@share"); }
    QString scheme() const override { return QStringLiteral("smb"); }

    QVector<FileInfo> list(const QString &, bool) const override {
        return {FileInfo::fromFields(m_root + QStringLiteral("/remote-only.txt"),
                                     QStringLiteral("remote-only.txt"), 3,
                                     QDateTime::fromSecsSinceEpoch(1000000), false,
                                     QFile::ReadOwner)};
    }
    bool isDir(const QString &path) const override { return cleanPath(path) == m_root; }
    bool exists(const QString &path) const override { return cleanPath(path) == m_root; }
    QString cleanPath(const QString &path) const override {
        QString clean = path;
        while (clean.size() > 1 && clean.endsWith(QLatin1Char('/')))
            clean.chop(1);
        return clean;
    }
    RenameResult rename(const QString &, const QString &, QString *) override {
        return RenameResult::Failed;
    }
    QString parentPath(const QString &path) const override {
        const QString clean = cleanPath(path);
        const int slash = clean.lastIndexOf(QLatin1Char('/'));
        if (slash < 0)
            return {};
        return slash == 0 ? QStringLiteral("/") : clean.left(slash);
    }

private:
    QString m_root;
};

bool listHasName(FileSystemModel *model, const QString &name) {
    for (int row = 0; row < model->rowCount(); ++row)
        if (model->fileInfoAt(row).name() == name)
            return true;
    return false;
}

} // namespace

TEST(ComputerPanelTest, ALocalPlacePickedFromTheViewDropsTheServerBehindTheTab) {
    QTemporaryDir local;
    ASSERT_TRUE(local.isValid());
    ASSERT_TRUE(QFile(QDir(local.path()).filePath(QStringLiteral("local-only.txt")))
                    .open(QIODevice::WriteOnly));

    const QString remoteDir = QStringLiteral("/share/docs");
    FilePanel panel;
    panel.resize(800, 500);
    panel.show();
    panel.connectTabTo(0, std::make_shared<FakeShare>(remoteDir), [](QString *) { return true; },
                       remoteDir, QStringLiteral("tester@share"), SavedConnection{},
                       FileSystemModel::AuthRetryFactory());
    settle(panel);
    ASSERT_TRUE(panel.model()->hasNetworkSession());

    panel.showComputer(
        {makeEntry(ComputerEntry::Kind::UserFolder, QStringLiteral("Documents"), local.path())});
    settle(panel);
    ASSERT_TRUE(panel.isComputerView());

    // What activating that row does. The place is on THIS machine, so the parked
    // server must not come back with the tab: restoring it listed the local path
    // through the remote provider, i.e. "tester@share : Documents".
    panel.navigateTo(local.path(), FilePanel::ParkedConnection::Drop);
    settle(panel);

    EXPECT_FALSE(panel.isComputerView());
    EXPECT_FALSE(panel.model()->hasNetworkSession());
    EXPECT_EQ(QDir(panel.model()->rootPath()).canonicalPath(), QDir(local.path()).canonicalPath());
    EXPECT_TRUE(listHasName(panel.model(), QStringLiteral("local-only.txt")));
}

TEST(ComputerPanelTest, ARemoteDestinationStillGetsTheParkedServerBack) {
    const QString remoteDir = QStringLiteral("/share/docs");
    FilePanel panel;
    panel.resize(800, 500);
    panel.show();
    panel.connectTabTo(0, std::make_shared<FakeShare>(remoteDir), [](QString *) { return true; },
                       remoteDir, QStringLiteral("tester@share"), SavedConnection{},
                       FileSystemModel::AuthRetryFactory());
    settle(panel);

    panel.showComputer(
        {makeEntry(ComputerEntry::Kind::Drive, QStringLiteral("C"), QStringLiteral("C:/"))});
    settle(panel);
    ASSERT_TRUE(panel.isComputerView());

    panel.navigateTo(remoteDir); // default: this tab is still a window onto the server
    settle(panel);
    EXPECT_TRUE(panel.model()->hasNetworkSession());
    EXPECT_TRUE(listHasName(panel.model(), QStringLiteral("remote-only.txt")));
}

TEST(ComputerPanelTest, DriveRowsShowUsedSpaceAndFoldersShowNone) {
    ComputerEntry drive =
        makeEntry(ComputerEntry::Kind::Drive, QStringLiteral("OS (C:)"), QStringLiteral("C:/"));
    drive.bytesTotal = 500LL * 1024 * 1024 * 1024; // 500 GB
    drive.bytesFree = 100LL * 1024 * 1024 * 1024;  // 100 GB free -> 400 GB used
    ComputerEntry folder = makeEntry(ComputerEntry::Kind::UserFolder,
                                     QStringLiteral("My Desktop"), QStringLiteral("C:/Users"));

    FilePanel panel;
    panel.resize(800, 500);
    panel.show();
    panel.showComputer({drive, folder});
    settle(panel);
    ASSERT_EQ(panel.model()->rowCount(), 2);

    const auto sizeAt = [&](int row) {
        return panel.model()
            ->data(panel.model()->index(row, FileSystemModel::SizeColumn), Qt::DisplayRole)
            .toString();
    };
    // One number, like every other row in the column; the capacity it is
    // measured against belongs on the status line.
    EXPECT_EQ(sizeAt(0), QStringLiteral("400.0 GB"));
    // A folder has no capacity to report, so it keeps the ordinary directory
    // marker until its size is actually counted.
    EXPECT_EQ(sizeAt(1), QStringLiteral("<DIR>"));
}

TEST(ComputerPanelTest, CountingAUserFolderFillsInItsSize) {
    QTemporaryDir home;
    ASSERT_TRUE(home.isValid());
    QFile payload(QDir(home.path()).filePath(QStringLiteral("payload.bin")));
    ASSERT_TRUE(payload.open(QIODevice::WriteOnly));
    ASSERT_EQ(payload.write(QByteArray(4096, 'x')), 4096);
    payload.close();

    FilePanel panel;
    panel.resize(800, 500);
    panel.show();
    panel.showComputer({makeEntry(ComputerEntry::Kind::UserFolder,
                                  QStringLiteral("My Desktop"), home.path())});
    settle(panel);
    ASSERT_EQ(panel.model()->rowCount(), 1);

    // The row's own path is "computer://folder/..." and the backend behind it
    // cannot walk a directory, so counting has to reach the real folder through
    // the local filesystem and file the answer back under the row.
    panel.calculateDirSizeForRow(0);
    const auto sizeText = [&] {
        return panel.model()
            ->data(panel.model()->index(0, FileSystemModel::SizeColumn), Qt::DisplayRole)
            .toString();
    };
    QElapsedTimer waited;
    waited.start();
    while (sizeText() == QStringLiteral("<DIR>") && waited.elapsed() < 5000)
        qApp->processEvents(QEventLoop::AllEvents, 10);

    EXPECT_NE(sizeText(), QStringLiteral("<DIR>"))
        << "the folder's size was never counted";
    EXPECT_EQ(sizeText(), QStringLiteral("4.0 KB"));
}

TEST(ComputerPanelTest, WithNothingToRebuildItBackFallsBackToTheDirectoryUnderneath) {
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

    // No computerViewRequested handler is connected here, so nothing can
    // assemble the rows. Back must then fall back to the directory the entry
    // carries rather than leaving the panel where it was -- and it must never
    // land on the synthetic root, which no backend can list.
    panel.goBack();
    settle(panel);
    EXPECT_FALSE(panel.isComputerView());
    EXPECT_EQ(QDir(panel.model()->rootPath()).canonicalPath(),
              QDir(first.path()).canonicalPath());
    EXPECT_FALSE(panel.model()->rootPath().startsWith(ComputerProvider::rootPath()));
}

TEST(ComputerPanelTest, TheViewNeverHandsOutItsSyntheticRootAsAWorkingDirectory) {
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

    // Around two dozen callers ask currentPath() for somewhere to act -- a new
    // folder, a copy destination, a terminal's cwd. "computer://" is not just
    // useless to them: QDir("computer://").filePath("x") is "computer:/x", and
    // creating that makes a real directory named "computer:" in the process's
    // working directory.
    EXPECT_FALSE(panel.currentPath().startsWith(ComputerProvider::rootPath()));
    EXPECT_EQ(QDir(panel.currentPath()).canonicalPath(), QDir(dir.path()).canonicalPath());
    EXPECT_TRUE(QDir(panel.currentPath()).exists());
    // The synthetic root is still what the model is listing -- only the answer
    // given to callers that need a real directory is different.
    EXPECT_EQ(panel.model()->rootPath(), ComputerProvider::rootPath());
}

TEST(ComputerPanelTest, NothingInTheViewIsSelectableAsAFile) {
    FilePanel panel;
    panel.resize(800, 500);
    panel.show();
    panel.showComputer({
        makeEntry(ComputerEntry::Kind::Drive, QStringLiteral("C"), QStringLiteral("C:/")),
        makeEntry(ComputerEntry::Kind::SavedServer, QStringLiteral("NAS"),
                  QStringLiteral("uuid-1")),
    });
    settle(panel);
    ASSERT_EQ(panel.model()->rowCount(), 2);

    panel.selectAll();
    qApp->processEvents();

    // Copy, move, delete, compress and checksum all begin by asking for the
    // selection. A "computer://drive/C:/" arriving in one of them is a
    // confusing failure at best; an empty selection makes them all do nothing,
    // which is the truth here -- these rows are places, not files.
    EXPECT_TRUE(panel.selectedPaths().isEmpty());
    EXPECT_TRUE(panel.selectedEntryInfos().isEmpty());
}

TEST(ComputerPanelTest, RefreshingTheViewReCollectsTheRows) {
    FilePanel panel;
    panel.resize(800, 500);
    panel.show();

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

    // Refresh reaches here from F5 and from the panel refresh that follows a
    // delete or a copy. Replaying the snapshot would keep showing a stick that
    // has since been pulled out.
    panel.refresh();
    settle(panel);
    EXPECT_EQ(rebuilds, 1);
    EXPECT_TRUE(panel.isComputerView());
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
