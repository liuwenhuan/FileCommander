#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFutureWatcher>
#include <QItemSelectionModel>
#include <QPointer>
#include <QSemaphore>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>
#include <memory>

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

    int folderRow = -1;
    for (int row = 0; row < panel.model()->rowCount(); ++row) {
        if (panel.model()->fileInfoAt(row).name() == QStringLiteral("folder")) {
            folderRow = row;
            break;
        }
    }
    ASSERT_GE(folderRow, 0);
    panel.view()->selectionModel()->select(
        panel.model()->index(folderRow, 0),
        QItemSelectionModel::Select | QItemSelectionModel::Rows);
    panel.calculateDirSizes();

    const auto watchers = panel.findChildren<QFutureWatcher<qint64> *>();
    ASSERT_EQ(watchers.size(), 1);
    QPointer<QFutureWatcher<qint64>> watcher = watchers.first();
    bool operationFinished = false;
    qint64 operationResult = -1;
    QObject::connect(watcher, &QFutureWatcher<qint64>::finished, &panel, [&]() {
        operationResult = watcher->result();
        operationFinished = true;
    });

    QTRY_VERIFY_WITH_TIMEOUT(gate->entered.load(), 4000);
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

    gate->release.release();
    QTRY_VERIFY_WITH_TIMEOUT(operationFinished, 4000);
    EXPECT_EQ(operationResult, 17);
    EXPECT_EQ(panel.currentPath(), localDir.path());
    EXPECT_EQ(visibleEntryNames(panel.model()), visibleBeforeClose);
    QTRY_VERIFY_WITH_TIMEOUT(weakShare.expired(), 4000);
}
