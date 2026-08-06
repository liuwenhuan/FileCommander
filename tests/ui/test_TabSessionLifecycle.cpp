#include <gtest/gtest.h>

#include "TryUntil.h"

#include <QAbstractButton>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QSemaphore>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>
#include <algorithm>
#include <memory>
#include <vector>

#include "diagnostics/RuntimeCounters.h"
#include "DirectorySizeTask.h"
#include "FilePanel.h"
#include "FileProvider.h"
#include "FileListView.h"
#include "FileSystemModel.h"
#include "SessionManager.h"
#include "TabBar.h"

namespace {

struct SizeGate {
    std::atomic<bool> entered{false};
    QSemaphore release{0};
};

class GateReleaseGuard {
public:
    explicit GateReleaseGuard(QSemaphore &semaphore) : m_semaphore(&semaphore) {}
    ~GateReleaseGuard() { release(); }

    void release() {
        if (!m_semaphore)
            return;
        m_semaphore->release();
        m_semaphore = nullptr;
    }

private:
    QSemaphore *m_semaphore;
};

class LifecycleShare : public FileProvider {
public:
    explicit LifecycleShare(QString root, std::shared_ptr<SizeGate> gate = {})
        : m_root(std::move(root)), m_gate(std::move(gate)) {}

    QString displayName() const override { return QStringLiteral("tester@share"); }
    QString scheme() const override { return QStringLiteral("smb"); }

    QVector<FileInfo> list(const QString &path, bool) const override {
        const QString clean = cleanPath(path);
        if (clean == m_root) {
            return {FileInfo::fromFields(m_root + QStringLiteral("/folder"),
                                         QStringLiteral("folder"), 0,
                                         QDateTime::fromSecsSinceEpoch(1000000), true,
                                         QFile::ReadOwner)};
        }
        if (clean == m_root + QStringLiteral("/folder")) {
            if (m_gate) {
                m_gate->entered.store(true);
                m_gate->release.acquire();
            }
            return {FileInfo::fromFields(clean + QStringLiteral("/payload.bin"),
                                         QStringLiteral("payload.bin"), 17,
                                         QDateTime::fromSecsSinceEpoch(1000000), false,
                                         QFile::ReadOwner)};
        }
        return {};
    }

    bool isDir(const QString &path) const override {
        const QString clean = cleanPath(path);
        return clean == m_root || clean == m_root + QStringLiteral("/folder");
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
        return clean == m_root || clean == m_root + QStringLiteral("/folder") ||
               clean == m_root + QStringLiteral("/folder/payload.bin");
    }

    RenameResult rename(const QString &, const QString &, QString *) override {
        return RenameResult::Failed;
    }

private:
    QString m_root;
    std::shared_ptr<SizeGate> m_gate;
};

class IsolatedConfigDir {
public:
    IsolatedConfigDir()
        : m_overrideWasSet(qEnvironmentVariableIsSet("FILECOMMANDER_CONFIG_HOME")),
          m_previousOverride(qgetenv("FILECOMMANDER_CONFIG_HOME")) {
        qputenv("FILECOMMANDER_CONFIG_HOME", m_dir.path().toUtf8());
    }

    ~IsolatedConfigDir() {
        if (m_overrideWasSet)
            qputenv("FILECOMMANDER_CONFIG_HOME", m_previousOverride);
        else
            qunsetenv("FILECOMMANDER_CONFIG_HOME");
    }

private:
    QTemporaryDir m_dir;
    bool m_overrideWasSet;
    QByteArray m_previousOverride;
};

void settle(FilePanel &panel) {
    QSignalSpy spy(panel.model(), &FileSystemModel::loadFinished);
    if (spy.isEmpty())
        spy.wait(4000);
    QCoreApplication::processEvents();
}

bool listHasName(FileSystemModel *model, const QString &name) {
    for (int row = 0; row < model->rowCount(); ++row)
        if (model->fileInfoAt(row).name() == name)
            return true;
    return false;
}

DirectorySizeTask *startBlockedDirectorySize(FilePanel &panel,
                                             const std::shared_ptr<SizeGate> &gate) {
    auto *view = panel.findChild<FileListView *>();
    if (!view)
        return nullptr;
    for (int row = 0; row < panel.model()->rowCount(); ++row) {
        if (panel.model()->fileInfoAt(row).name() != QStringLiteral("folder"))
            continue;
        view->setCurrentIndex(panel.model()->index(row, FileSystemModel::NameColumn));
        panel.calculateDirSizes();
        QElapsedTimer elapsed;
        elapsed.start();
        while (!gate->entered.load() && elapsed.elapsed() < 5000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        if (!gate->entered.load())
            return nullptr;
        return panel.findChild<DirectorySizeTask *>();
    }
    return nullptr;
}

bool waitForCancelledDirectorySize(QSignalSpy &finished) {
    if (finished.isEmpty() && !finished.wait(4000))
        return false;
    return finished.count() == 1 && finished.first().size() == 3 &&
           finished.first().at(2).toBool();
}

QStringList visibleEntryNames(FileSystemModel *model) {
    QStringList names;
    for (int row = 0; row < model->rowCount(); ++row) {
        if (!model->isParentEntry(row))
            names.append(model->fileInfoAt(row).name());
    }
    return names;
}

bool writeFile(const QString &path, const QByteArray &contents) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    return file.write(contents) == contents.size();
}

SavedConnection savedConnection(const QString &path) {
    SavedConnection saved;
    saved.id = QStringLiteral("saved-share");
    saved.name = QStringLiteral("Saved share");
    saved.protocol = static_cast<int>(ConnectionProtocol::Smb);
    saved.host = QStringLiteral("nas.example");
    saved.port = 445;
    saved.user = QStringLiteral("tester");
    saved.remotePath = path;
    return saved;
}

constexpr int kRuntimeWaitMs = 5000;

bool sameRuntimeSnapshot(const fc::RuntimeSnapshot &lhs, const fc::RuntimeSnapshot &rhs) {
    return lhs.networkSessions == rhs.networkSessions &&
           lhs.networkThreads == rhs.networkThreads &&
           lhs.activeHeartbeats == rhs.activeHeartbeats &&
           lhs.transferWorkers == rhs.transferWorkers &&
           lhs.curlTransfers == rhs.curlTransfers;
}

QString runtimeSnapshotText(const fc::RuntimeSnapshot &snapshot) {
    return QStringLiteral("{networkSessions=%1, networkThreads=%2, activeHeartbeats=%3, "
                          "transferWorkers=%4, curlTransfers=%5}")
        .arg(snapshot.networkSessions)
        .arg(snapshot.networkThreads)
        .arg(snapshot.activeHeartbeats)
        .arg(snapshot.transferWorkers)
        .arg(snapshot.curlTransfers);
}

void recordRuntimePeak(fc::RuntimeSnapshot &peak) {
    const fc::RuntimeSnapshot current = fc::runtimeSnapshot();
    peak.networkSessions = std::max(peak.networkSessions, current.networkSessions);
    peak.networkThreads = std::max(peak.networkThreads, current.networkThreads);
    peak.activeHeartbeats = std::max(peak.activeHeartbeats, current.activeHeartbeats);
    peak.transferWorkers = std::max(peak.transferWorkers, current.transferWorkers);
    peak.curlTransfers = std::max(peak.curlTransfers, current.curlTransfers);
}

template <typename Predicate>
bool waitForCondition(Predicate predicate, int timeoutMs = kRuntimeWaitMs) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate()) {
        if (timer.elapsed() >= timeoutMs)
            return predicate();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    return true;
}

bool requireScenario(bool condition, const QString &reason) {
    if (!condition)
        ADD_FAILURE() << reason.toStdString();
    return condition;
}

bool waitForRemoteListing(FilePanel &panel, const QString &remoteDir,
                          QSignalSpy &listingFinished) {
    return waitForCondition([&panel, &remoteDir, &listingFinished] {
        return listingFinished.count() > 0 && panel.model()->hasNetworkSession() &&
               panel.currentPath() == remoteDir &&
               listHasName(panel.model(), QStringLiteral("folder"));
    });
}

bool waitForRuntimeResources(const fc::RuntimeSnapshot &baseline, int remoteTabCount) {
    return waitForCondition([&baseline, remoteTabCount] {
        const fc::RuntimeSnapshot current = fc::runtimeSnapshot();
        return current.networkSessions == baseline.networkSessions + remoteTabCount &&
               current.networkThreads == baseline.networkThreads + remoteTabCount &&
               current.activeHeartbeats == baseline.activeHeartbeats + remoteTabCount &&
               current.transferWorkers == baseline.transferWorkers &&
               current.curlTransfers == baseline.curlTransfers;
    });
}

struct RuntimeScenarioResult {
    bool completed = false;
    fc::RuntimeSnapshot peak;
};

RuntimeScenarioResult runTenRemoteTabScenario(const fc::RuntimeSnapshot &baseline) {
    constexpr int remoteTabCount = 10;
    fc::RuntimeSnapshot peak = baseline;
    std::vector<std::shared_ptr<LifecycleShare>> shares;
    QStringList remoteDirs;
    shares.reserve(remoteTabCount);
    remoteDirs.reserve(remoteTabCount);

    // All owners live in this scope. Returning on a failed bounded wait destroys
    // the panel and provider vector before the outer test checks final resources.
    auto panel = std::make_unique<FilePanel>();
    for (int i = 0; i < remoteTabCount; ++i) {
        if (i > 0)
            panel->newTab();
        if (!requireScenario(panel->tabCount() == i + 1,
                             QStringLiteral("tab count after creating remote tab %1").arg(i)))
            return {false, peak};

        const QString remoteDir = QStringLiteral("/share/docs%1").arg(i);
        remoteDirs.append(remoteDir);
        auto share = std::make_shared<LifecycleShare>(remoteDir);
        shares.push_back(share);
        QSignalSpy listingFinished(panel->model(), &FileSystemModel::loadFinished);
        panel->connectTabTo(i, share, [](QString *) { return true; }, remoteDir,
                            QStringLiteral("tester@share%1").arg(i), SavedConnection{},
                            FileSystemModel::AuthRetryFactory());

        if (!requireScenario(waitForRemoteListing(*panel, remoteDir, listingFinished),
                             QStringLiteral("remote listing ready for tab %1").arg(i)) ||
            !requireScenario(waitForRuntimeResources(baseline, i + 1),
                             QStringLiteral("runtime resources reached %1 remote tabs").arg(i + 1)))
            return {false, peak};
        if (!requireScenario(panel->tabHasConnection(i),
                             QStringLiteral("remote connection metadata for tab %1").arg(i)))
            return {false, peak};
        recordRuntimePeak(peak);
    }

    // Add a local survivor so every remote tab, including the last one, can be
    // closed through the real close-button path.
    panel->newTab();
    if (!requireScenario(
            waitForCondition([&] {
                return panel->tabCount() == remoteTabCount + 1 &&
                       !panel->model()->hasNetworkSession();
            }),
            QStringLiteral("local survivor ready")) ||
        !requireScenario(waitForRuntimeResources(baseline, remoteTabCount),
                         QStringLiteral("parked resources remain live with local survivor")))
        return {false, peak};
    recordRuntimePeak(peak);

    // Exercise both active and parked ownership before closing anything.
    for (int i = 0; i < remoteTabCount; ++i) {
        QSignalSpy listingFinished(panel->model(), &FileSystemModel::loadFinished);
        panel->activateTab(i);
        if (!requireScenario(waitForRemoteListing(*panel, remoteDirs.at(i), listingFinished),
                             QStringLiteral("remote listing restored for tab %1").arg(i)) ||
            !requireScenario(waitForRuntimeResources(baseline, remoteTabCount),
                             QStringLiteral("all ten resources remain live while tab %1 is active")
                                 .arg(i)))
            return {false, peak};
        recordRuntimePeak(peak);
    }
    panel->activateTab(remoteTabCount);
    if (!requireScenario(
            waitForCondition([&] { return !panel->model()->hasNetworkSession(); }),
            QStringLiteral("local survivor restored after activation sweep")) ||
        !requireScenario(waitForRuntimeResources(baseline, remoteTabCount),
                         QStringLiteral("all ten resources remain live while parked")))
        return {false, peak};
    recordRuntimePeak(peak);

    auto *tabs = panel->findChild<TabBar *>();
    if (!requireScenario(tabs != nullptr, QStringLiteral("tab bar exists for close-button path")))
        return {false, peak};
    for (int i = remoteTabCount - 1; i >= 0; --i) {
        auto *closeButton = qobject_cast<QAbstractButton *>(
            tabs->tabButton(i, QTabBar::RightSide));
        if (!requireScenario(closeButton != nullptr && closeButton->isEnabled(),
                             QStringLiteral("enabled close button exists for tab %1").arg(i)))
            return {false, peak};

        QSignalSpy closeRequested(tabs, &QTabBar::tabCloseRequested);
        closeButton->click();
        if (!requireScenario(
                waitForCondition([&] {
                    return closeRequested.count() == 1 && panel->tabCount() == i + 1;
                }),
                QStringLiteral("close button routed through QTabBar for tab %1").arg(i)) ||
            !requireScenario(waitForRuntimeResources(baseline, i),
                             QStringLiteral("runtime resources drained after closing tab %1").arg(i)))
            return {false, peak};
        if (!requireScenario(!panel->tabHasConnection(i),
                             QStringLiteral("closed tab %1 no longer has a connection").arg(i)))
            return {false, peak};
        recordRuntimePeak(peak);
    }

    if (!requireScenario(panel->tabCount() == 1 && !panel->tabHasConnection(0),
                         QStringLiteral("only local survivor remains after remote closes")))
        return {false, peak};
    return {true, peak};
}

} // namespace

TEST(TabSessionLifecycle, NetworkTabIsParkedAndAdoptedAcrossSwitch) {
    const QString remoteDir = QStringLiteral("/share/docs");
    FilePanel panel;
    auto share = std::make_shared<LifecycleShare>(remoteDir);
    panel.connectTabTo(0, share, [](QString *) { return true; }, remoteDir,
                       QStringLiteral("tester@share"), SavedConnection{},
                       FileSystemModel::AuthRetryFactory());
    settle(panel);

    ASSERT_TRUE(panel.model()->hasNetworkSession());

    panel.newTab();
    settle(panel);
    EXPECT_FALSE(panel.model()->hasNetworkSession());

    panel.prevTab();
    settle(panel);
    EXPECT_TRUE(panel.model()->hasNetworkSession());
    EXPECT_EQ(panel.currentPath(), remoteDir);
    EXPECT_TRUE(listHasName(panel.model(), QStringLiteral("folder")));
}

TEST(TabSessionLifecycle, InactiveNetworkTabCanBeClosedWithoutAffectingLocalTab) {
    const QString remoteDir = QStringLiteral("/share/docs");
    QTemporaryDir localDir;
    ASSERT_TRUE(localDir.isValid());
    ASSERT_TRUE(writeFile(QDir(localDir.path()).filePath(QStringLiteral("survivor.txt")),
                          QByteArrayLiteral("local content")));

    FilePanel panel;
    auto share = std::make_shared<LifecycleShare>(remoteDir);
    panel.connectTabTo(0, share, [](QString *) { return true; }, remoteDir,
                       QStringLiteral("tester@share"), SavedConnection{},
                       FileSystemModel::AuthRetryFactory());
    settle(panel);

    panel.newTab();
    panel.navigateTo(localDir.path());
    settle(panel);
    ASSERT_EQ(panel.activeTabIndex(), 1);
    ASSERT_EQ(panel.currentPath(), localDir.path());
    const QStringList visibleBefore = visibleEntryNames(panel.model());
    ASSERT_EQ(visibleBefore, QStringList{QStringLiteral("survivor.txt")});

    auto *tabs = panel.findChild<TabBar *>();
    ASSERT_NE(tabs, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(tabs, "closeTabRequested", Qt::DirectConnection,
                                          Q_ARG(int, 0)));
    settle(panel);

    EXPECT_EQ(panel.tabCount(), 1);
    EXPECT_FALSE(panel.tabHasConnection(0));
    EXPECT_FALSE(panel.model()->hasNetworkSession());
    EXPECT_EQ(panel.currentPath(), localDir.path());
    EXPECT_EQ(visibleEntryNames(panel.model()), visibleBefore);
}

TEST(TabSessionLifecycle, ExchangeMovesRemoteBackendWithItsLocation) {
    const QString remoteDir = QStringLiteral("/share/docs");
    QTemporaryDir localDir;
    ASSERT_TRUE(localDir.isValid());

    FilePanel left;
    FilePanel right;
    auto share = std::make_shared<LifecycleShare>(remoteDir);
    left.connectTabTo(0, share, [](QString *) { return true; }, remoteDir,
                      QStringLiteral("tester@share"), SavedConnection{},
                      FileSystemModel::AuthRetryFactory());
    settle(left);
    right.navigateTo(localDir.path());
    settle(right);
    ASSERT_TRUE(left.model()->hasNetworkSession());
    const QString connectionId = left.connectionId();

    left.exchangeLocationWith(&right);
    settle(left);
    settle(right);

    EXPECT_EQ(left.currentPath(), localDir.path());
    EXPECT_FALSE(left.model()->hasNetworkSession());
    EXPECT_EQ(right.currentPath(), remoteDir);
    EXPECT_TRUE(right.model()->hasNetworkSession());
    EXPECT_EQ(right.connectionId(), connectionId);
    EXPECT_TRUE(listHasName(right.model(), QStringLiteral("folder")));
}

TEST(TabSessionLifecycle, ExchangeInvalidatesRunningDirectorySizeGeneration) {
    const QString remoteDir = QStringLiteral("/share/docs");
    QTemporaryDir localDir;
    ASSERT_TRUE(localDir.isValid());
    auto gate = std::make_shared<SizeGate>();
    auto share = std::make_shared<LifecycleShare>(remoteDir, gate);

    FilePanel left;
    FilePanel right;
    left.connectTabTo(0, share, [](QString *) { return true; }, remoteDir,
                      QStringLiteral("tester@share"), SavedConnection{},
                      FileSystemModel::AuthRetryFactory());
    settle(left);
    right.navigateTo(localDir.path());
    settle(right);

    DirectorySizeTask *task = startBlockedDirectorySize(left, gate);
    ASSERT_NE(task, nullptr);
    QSignalSpy finished(task, &DirectorySizeTask::finished);
    GateReleaseGuard releaseGate(gate->release);

    left.exchangeLocationWith(&right);
    releaseGate.release();

    EXPECT_TRUE(waitForCancelledDirectorySize(finished));
}

TEST(TabSessionLifecycle, OpenLocalInvalidatesRunningDirectorySizeGeneration) {
    const QString remoteDir = QStringLiteral("/share/docs");
    QTemporaryDir localDir;
    ASSERT_TRUE(localDir.isValid());
    auto gate = std::make_shared<SizeGate>();
    auto share = std::make_shared<LifecycleShare>(remoteDir, gate);

    FilePanel panel;
    panel.connectTabTo(0, share, [](QString *) { return true; }, remoteDir,
                       QStringLiteral("tester@share"), SavedConnection{},
                       FileSystemModel::AuthRetryFactory());
    settle(panel);

    DirectorySizeTask *task = startBlockedDirectorySize(panel, gate);
    ASSERT_NE(task, nullptr);
    QSignalSpy finished(task, &DirectorySizeTask::finished);
    GateReleaseGuard releaseGate(gate->release);

    panel.openLocalInTab(-1, localDir.path());
    releaseGate.release();

    EXPECT_TRUE(waitForCancelledDirectorySize(finished));
}

TEST(TabSessionLifecycle, NewTabInvalidatesRunningDirectorySizeGeneration) {
    const QString remoteDir = QStringLiteral("/share/docs");
    auto gate = std::make_shared<SizeGate>();
    auto share = std::make_shared<LifecycleShare>(remoteDir, gate);

    FilePanel panel;
    panel.connectTabTo(0, share, [](QString *) { return true; }, remoteDir,
                       QStringLiteral("tester@share"), SavedConnection{},
                       FileSystemModel::AuthRetryFactory());
    settle(panel);

    DirectorySizeTask *task = startBlockedDirectorySize(panel, gate);
    ASSERT_NE(task, nullptr);
    QSignalSpy finished(task, &DirectorySizeTask::finished);
    GateReleaseGuard releaseGate(gate->release);

    panel.newTab();
    releaseGate.release();

    EXPECT_TRUE(waitForCancelledDirectorySize(finished));
}

TEST(TabSessionLifecycle, SearchResultsInvalidateRunningDirectorySizeGeneration) {
    const QString remoteDir = QStringLiteral("/share/docs");
    auto gate = std::make_shared<SizeGate>();
    auto share = std::make_shared<LifecycleShare>(remoteDir, gate);

    FilePanel panel;
    panel.connectTabTo(0, share, [](QString *) { return true; }, remoteDir,
                       QStringLiteral("tester@share"), SavedConnection{},
                       FileSystemModel::AuthRetryFactory());
    settle(panel);

    DirectorySizeTask *task = startBlockedDirectorySize(panel, gate);
    ASSERT_NE(task, nullptr);
    QSignalSpy finished(task, &DirectorySizeTask::finished);
    GateReleaseGuard releaseGate(gate->release);

    panel.showSearchResultsInNewTab(
        QStringLiteral("payload"),
        {remoteDir + QStringLiteral("/folder/payload.bin")});
    releaseGate.release();

    EXPECT_TRUE(waitForCancelledDirectorySize(finished));
}

TEST(TabSessionLifecycle, SavedRemoteTabMetadataRoundTripsThroughSessionRestore) {
    IsolatedConfigDir isolatedConfig;
    const QString remoteDir = QStringLiteral("/share/docs");
    const SavedConnection saved = savedConnection(remoteDir);

    SessionTabData remoteTab;
    remoteTab.path = remoteDir;
    remoteTab.selectedFiles = QStringList{remoteDir + QStringLiteral("/folder")};
    remoteTab.conn = saved;
    SessionTabData localTab;
    localTab.path = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);

    SessionPanelData savedLeft;
    savedLeft.tabs = {remoteTab, localTab};
    savedLeft.activeTab = 0;
    SessionPanelData savedRight;
    savedRight.tabs = {localTab};
    savedRight.activeTab = 0;
    SessionManager::save(savedLeft, savedRight);

    SessionPanelData restoredLeft;
    SessionPanelData restoredRight;
    ASSERT_TRUE(SessionManager::load(restoredLeft, restoredRight));
    ASSERT_EQ(restoredLeft.tabs.size(), 2);
    EXPECT_EQ(restoredLeft.activeTab, 0);

    const SessionTabData &restoredRemote = restoredLeft.tabs.at(0);
    EXPECT_EQ(restoredRemote.path, remoteDir);
    EXPECT_EQ(restoredRemote.selectedFiles,
              QStringList{remoteDir + QStringLiteral("/folder")});
    EXPECT_FALSE(restoredRemote.conn.host.isEmpty());
    EXPECT_EQ(restoredRemote.conn.protocol, static_cast<int>(ConnectionProtocol::Smb));
    EXPECT_EQ(restoredRemote.conn.host, QStringLiteral("nas.example"));
    EXPECT_EQ(restoredRemote.conn.port, 445);
    EXPECT_EQ(restoredRemote.conn.user, QStringLiteral("tester"));
    EXPECT_EQ(restoredRemote.conn.remotePath, remoteDir);
    EXPECT_EQ(restoredRemote.conn.id, QStringLiteral("saved-share"));
}

TEST(TabSessionLifecycle, ProviderSizeTaskCompletesAfterRemoteTabCloses) {
    const QString remoteDir = QStringLiteral("/share/docs");
    QTemporaryDir localDir;
    ASSERT_TRUE(localDir.isValid());
    ASSERT_TRUE(writeFile(QDir(localDir.path()).filePath(QStringLiteral("local.txt")),
                          QByteArrayLiteral("visible local state")));

    auto gate = std::make_shared<SizeGate>();
    auto share = std::make_shared<LifecycleShare>(remoteDir, gate);
    std::weak_ptr<FileProvider> weakShare = share;

    FilePanel panel;
    panel.connectTabTo(0, share, [](QString *) { return true; }, remoteDir,
                       QStringLiteral("tester@share"), SavedConnection{},
                       FileSystemModel::AuthRetryFactory());
    settle(panel);
    ASSERT_TRUE(panel.model()->hasNetworkSession());

    {
        constexpr quint64 requestId = 73;
        DirectorySizeTask task(requestId, share,
                               {remoteDir + QStringLiteral("/folder")});
        QSignalSpy finished(&task, &DirectorySizeTask::finished);
        task.start();

        FC_TRY_VERIFY_WITH_TIMEOUT(gate->entered.load(), 4000);
        GateReleaseGuard releaseGate(gate->release);
        panel.newTab();
        panel.navigateTo(localDir.path());
        settle(panel);
        EXPECT_FALSE(panel.model()->hasNetworkSession());
        ASSERT_EQ(panel.currentPath(), localDir.path());
        const QStringList visibleBeforeClose = visibleEntryNames(panel.model());
        ASSERT_EQ(visibleBeforeClose, QStringList{QStringLiteral("local.txt")});

        auto *tabs = panel.findChild<TabBar *>();
        ASSERT_NE(tabs, nullptr);
        ASSERT_TRUE(QMetaObject::invokeMethod(tabs, "closeTabRequested", Qt::DirectConnection,
                                              Q_ARG(int, 0)));
        settle(panel);

        share.reset();
        EXPECT_EQ(panel.tabCount(), 1);
        EXPECT_EQ(panel.currentPath(), localDir.path());
        EXPECT_EQ(visibleEntryNames(panel.model()), visibleBeforeClose);

        releaseGate.release();
        ASSERT_TRUE(finished.wait(4000));
        ASSERT_EQ(finished.count(), 1);
        const QList<QVariant> result = finished.takeFirst();
        EXPECT_EQ(result.at(0).toULongLong(), requestId);
        EXPECT_EQ(result.at(1).toLongLong(), 17);
        EXPECT_FALSE(result.at(2).toBool());
        EXPECT_EQ(panel.currentPath(), localDir.path());
        EXPECT_EQ(visibleEntryNames(panel.model()), visibleBeforeClose);
    }
    FC_TRY_VERIFY_WITH_TIMEOUT(weakShare.expired(), 4000);
}

TEST(TabSessionLifecycle, TenRemoteTabsReturnRuntimeResourcesToBaseline) {
    constexpr int remoteTabCount = 10;
    const fc::RuntimeSnapshot baseline = fc::runtimeSnapshot();
    const RuntimeScenarioResult scenario = runTenRemoteTabScenario(baseline);
    const bool returnedToBaseline = waitForCondition(
        [&] { return sameRuntimeSnapshot(fc::runtimeSnapshot(), baseline); });
    const fc::RuntimeSnapshot finalSnapshot = fc::runtimeSnapshot();
    EXPECT_TRUE(scenario.completed);
    EXPECT_TRUE(returnedToBaseline)
        << "final=" << runtimeSnapshotText(finalSnapshot).toStdString()
        << " baseline=" << runtimeSnapshotText(baseline).toStdString();

    EXPECT_EQ(scenario.peak.networkSessions, baseline.networkSessions + remoteTabCount);
    EXPECT_EQ(scenario.peak.networkThreads, baseline.networkThreads + remoteTabCount);
    EXPECT_EQ(scenario.peak.activeHeartbeats, baseline.activeHeartbeats + remoteTabCount);
    EXPECT_EQ(scenario.peak.transferWorkers, baseline.transferWorkers);
    EXPECT_EQ(scenario.peak.curlTransfers, baseline.curlTransfers);

    EXPECT_EQ(finalSnapshot.networkSessions, baseline.networkSessions);
    EXPECT_EQ(finalSnapshot.networkThreads, baseline.networkThreads);
    EXPECT_EQ(finalSnapshot.activeHeartbeats, baseline.activeHeartbeats);
    EXPECT_EQ(finalSnapshot.transferWorkers, baseline.transferWorkers);
    EXPECT_EQ(finalSnapshot.curlTransfers, baseline.curlTransfers);
}
