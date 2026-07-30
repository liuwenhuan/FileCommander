#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include "DirectorySizeTask.h"
#include "FileProvider.h"

namespace {

class BlockingProvider final : public FileProvider {
public:
    QVector<FileInfo> list(const QString &path, bool) const override {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_listedPaths.append(path);
        if (path == QStringLiteral("/first")) {
            m_firstRootEntered = true;
            m_entered.notify_all();
            m_release.wait(lock, [this] { return m_firstRootReleased; });
        }
        return {};
    }

    bool isDir(const QString &) const override { return true; }
    QString cleanPath(const QString &path) const override { return path; }
    QString parentPath(const QString &) const override { return {}; }
    bool exists(const QString &) const override { return true; }
    RenameResult rename(const QString &, const QString &, QString *) override {
        return RenameResult::Failed;
    }

    bool waitUntilFirstRootEntered(std::chrono::milliseconds timeout =
                                       std::chrono::milliseconds(4000)) const {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_entered.wait_for(lock, timeout, [this] { return m_firstRootEntered; });
    }

    void releaseFirstRoot() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_firstRootReleased = true;
        m_release.notify_all();
    }

    QStringList listedPaths() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_listedPaths;
    }

private:
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_entered;
    mutable std::condition_variable m_release;
    mutable bool m_firstRootEntered = false;
    mutable bool m_firstRootReleased = false;
    mutable QStringList m_listedPaths;
};

class SymlinkRootProvider final : public FileProvider {
public:
    explicit SymlinkRootProvider(QString root) : m_root(std::move(root)) {}

    QVector<FileInfo> list(const QString &path, bool) const override {
        if (path == parentPath(m_root))
            return {FileInfo(m_root)};
        if (path == m_root) {
            ++m_rootListCount;
            return {FileInfo::fromFields(QDir(m_root).filePath(QStringLiteral("inside.bin")),
                                         QStringLiteral("inside.bin"), 10, {}, false, {})};
        }
        return {};
    }

    bool isDir(const QString &) const override { return true; }
    QString cleanPath(const QString &path) const override { return QDir::cleanPath(path); }
    QString parentPath(const QString &path) const override { return QFileInfo(path).absolutePath(); }
    bool exists(const QString &) const override { return true; }
    RenameResult rename(const QString &, const QString &, QString *) override {
        return RenameResult::Failed;
    }

    int rootListCount() const { return m_rootListCount; }

private:
    QString m_root;
    mutable int m_rootListCount = 0;
};

} // namespace

TEST(DirectorySizeTask, CancelStopsBeforeTheNextRoot) {
    auto provider = std::make_shared<BlockingProvider>();
    DirectorySizeTask task(42, provider, {QStringLiteral("/first"), QStringLiteral("/second")});
    QSignalSpy finished(&task, &DirectorySizeTask::finished);

    task.start();
    ASSERT_TRUE(provider->waitUntilFirstRootEntered());
    task.cancel();
    provider->releaseFirstRoot();

    ASSERT_TRUE(finished.wait(4000));
    ASSERT_EQ(finished.count(), 1);
    const QList<QVariant> result = finished.takeFirst();
    EXPECT_EQ(result.at(0).toULongLong(), 42u);
    EXPECT_EQ(result.at(1).toLongLong(), 0);
    EXPECT_TRUE(result.at(2).toBool());
    EXPECT_EQ(provider->listedPaths(), (QStringList{QStringLiteral("/first")}));
}

TEST(DirectorySizeTask, DoesNotTraverseASymlinkDirectoryRoot) {
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString target = QDir(temp.path()).filePath(QStringLiteral("target"));
    const QString link = QDir(temp.path()).filePath(QStringLiteral("link.lnk"));
    ASSERT_TRUE(QDir().mkdir(target));
    if (!QFile::link(target, link))
        GTEST_SKIP() << "filesystem does not support directory symlinks";
    ASSERT_TRUE(QFileInfo(link).isSymLink());

    auto provider = std::make_shared<SymlinkRootProvider>(link);
    DirectorySizeTask task(43, provider, {link});
    QSignalSpy finished(&task, &DirectorySizeTask::finished);

    task.start();

    ASSERT_TRUE(finished.wait(4000));
    EXPECT_EQ(provider->rootListCount(), 0);
    ASSERT_EQ(finished.count(), 1);
    EXPECT_EQ(finished.takeFirst().at(1).toLongLong(), 0);
}

TEST(DirectorySizeTask, DestructionWhileProviderIsBlockedCompletesAfterUnblock) {
    auto provider = std::make_shared<BlockingProvider>();
    const std::weak_ptr<BlockingProvider> providerLifetime = provider;
    auto *task =
        new DirectorySizeTask(44, provider, {QStringLiteral("/first"), QStringLiteral("/second")});
    task->start();
    ASSERT_TRUE(provider->waitUntilFirstRootEntered());

    std::thread unblocker([provider] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        provider->releaseFirstRoot();
    });
    QElapsedTimer timer;
    timer.start();
    delete task;
    unblocker.join();

    EXPECT_LT(timer.elapsed(), 1000);
    QCoreApplication::processEvents();
    EXPECT_EQ(provider->listedPaths(), (QStringList{QStringLiteral("/first")}));

    provider.reset();
    QElapsedTimer cleanupTimer;
    cleanupTimer.start();
    while (!providerLifetime.expired() && cleanupTimer.elapsed() < 4000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    EXPECT_TRUE(providerLifetime.expired());
}
