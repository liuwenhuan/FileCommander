#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QItemSelectionModel>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QSemaphore>
#include <QTest>

#include <atomic>
#include <memory>

#include "FilePanel.h"
#include "FileProvider.h"
#include "FileListView.h"
#include "FileSystemModel.h"
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
    FilePanel panel;
    auto share = std::make_shared<LifecycleShare>(remoteDir);
    panel.connectTabTo(0, share, [](QString *) { return true; }, remoteDir,
                       QStringLiteral("tester@share"), SavedConnection{},
                       FileSystemModel::AuthRetryFactory());
    settle(panel);

    panel.newTab();
    settle(panel);
    ASSERT_EQ(panel.activeTabIndex(), 1);

    auto *tabs = panel.findChild<TabBar *>();
    ASSERT_NE(tabs, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(tabs, "closeTabRequested", Qt::DirectConnection,
                                          Q_ARG(int, 0)));
    settle(panel);

    EXPECT_EQ(panel.tabCount(), 1);
    EXPECT_FALSE(panel.tabHasConnection(0));
    EXPECT_FALSE(panel.model()->hasNetworkSession());
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

TEST(TabSessionLifecycle, RestoredSavedRemoteTabKeepsReconnectDescriptor) {
    const QString remoteDir = QStringLiteral("/share/docs");
    FilePanel panel;
    panel.restoreTabs({{remoteDir, {}},
                       {QStandardPaths::writableLocation(QStandardPaths::HomeLocation), {}}},
                      1);
    ASSERT_EQ(panel.tabCount(), 2);
    ASSERT_EQ(panel.activeTabIndex(), 1);

    const SavedConnection saved = savedConnection(remoteDir);
    auto share = std::make_shared<LifecycleShare>(remoteDir);
    panel.connectTabTo(0, share, [](QString *) { return true; }, remoteDir,
                       QStringLiteral("tester@nas.example"), saved,
                       FileSystemModel::AuthRetryFactory());
    settle(panel);
    ASSERT_EQ(panel.tabConnInfo(0).host, saved.host);
    ASSERT_EQ(panel.tabConnInfo(0).remotePath, saved.remotePath);

    panel.activateTab(1);
    settle(panel);
    EXPECT_FALSE(panel.model()->hasNetworkSession());
    panel.activateTab(0);
    settle(panel);
    EXPECT_TRUE(panel.model()->hasNetworkSession());
    EXPECT_EQ(panel.currentPath(), remoteDir);
    EXPECT_EQ(panel.tabConnInfo(0).id, saved.id);
}

TEST(TabSessionLifecycle, ProviderSizeTaskSurvivesSwitchingAwayFromRemoteTab) {
    const QString remoteDir = QStringLiteral("/share/docs");
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

    QTRY_VERIFY_WITH_TIMEOUT(gate->entered.load(), 4000);
    panel.newTab();
    settle(panel);
    EXPECT_FALSE(panel.model()->hasNetworkSession());

    auto *tabs = panel.findChild<TabBar *>();
    ASSERT_NE(tabs, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(tabs, "closeTabRequested", Qt::DirectConnection,
                                          Q_ARG(int, 0)));
    settle(panel);

    share.reset();
    EXPECT_FALSE(weakShare.expired()) << "the in-flight provider task lost its shared provider";

    gate->release.release();
    QTRY_VERIFY_WITH_TIMEOUT(weakShare.expired(), 4000);
}
