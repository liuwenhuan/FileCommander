#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include "DirectorySizeTask.h"
#include "FileProvider.h"
#include "LocalFileProvider.h"

namespace {

#ifdef Q_OS_WIN
QString extendedPath(const QString &path) {
    QString native = QDir::toNativeSeparators(QDir::cleanPath(path));
    if (native.startsWith(QStringLiteral("\\\\?\\")))
        return native;
    if (native.startsWith(QStringLiteral("\\\\")))
        return QStringLiteral("\\\\?\\UNC\\") + native.mid(2);
    return QStringLiteral("\\\\?\\") + native;
}

void createDirectoryWin32(const QString &path) {
    const std::wstring wide = extendedPath(path).toStdWString();
    ASSERT_TRUE(CreateDirectoryW(wide.c_str(), nullptr) ||
                GetLastError() == ERROR_ALREADY_EXISTS)
        << path.toStdString();
}

void writeFileWin32(const QString &path, const QByteArray &payload) {
    const std::wstring wide = extendedPath(path).toStdWString();
    HANDLE handle = CreateFileW(wide.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE) << path.toStdString();
    DWORD written = 0;
    ASSERT_TRUE(WriteFile(handle, payload.constData(), static_cast<DWORD>(payload.size()),
                          &written, nullptr));
    CloseHandle(handle);
    ASSERT_EQ(written, static_cast<DWORD>(payload.size()));
}
#endif

void writePayload(const QString &path, int bytes = 1) {
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write(QByteArray(bytes, 'x')), bytes);
}

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
        m_listReturned = true;
        m_returned.notify_all();
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

    bool waitUntilListReturned(std::chrono::milliseconds timeout =
                                   std::chrono::milliseconds(4000)) const {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_returned.wait_for(lock, timeout, [this] { return m_listReturned; });
    }

    QStringList listedPaths() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_listedPaths;
    }

private:
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_entered;
    mutable std::condition_variable m_release;
    mutable std::condition_variable m_returned;
    mutable bool m_firstRootEntered = false;
    mutable bool m_firstRootReleased = false;
    mutable bool m_listReturned = false;
    mutable QStringList m_listedPaths;
};

class SymlinkRootProvider final : public FileProvider {
public:
    QVector<FileInfo> list(const QString &path, bool) const override {
        if (path == QStringLiteral("/link")) {
            ++m_rootListCount;
            return {FileInfo::fromFields(QStringLiteral("/link/inside.bin"),
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
    mutable int m_rootListCount = 0;
};

} // namespace

TEST(DirectorySizeTaskTest, DirectorySizeTask_CancelStopsBeforeTheNextRoot) {
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

TEST(DirectorySizeTaskTest, DirectorySizeTask_DoesNotTraverseASymlinkDirectoryRoot) {
    auto provider = std::make_shared<SymlinkRootProvider>();
    DirectorySizeTask task(43, provider, {QStringLiteral("/link")}, nullptr,
                           {{QStringLiteral("/link"), 37}});
    QSignalSpy finished(&task, &DirectorySizeTask::finished);

    task.start();

    ASSERT_TRUE(finished.wait(4000));
    EXPECT_EQ(provider->rootListCount(), 0);
    ASSERT_EQ(finished.count(), 1);
    EXPECT_EQ(finished.takeFirst().at(1).toLongLong(), 37);
}

TEST(DirectorySizeTaskTest, DirectorySizeTask_SingleLocalRootUsesOneWorker) {
    EXPECT_EQ(DirectorySizeTask::localConcurrencyLimitForRoots({QStringLiteral("/only")}), 1);
}

TEST(DirectorySizeTaskTest, DirectorySizeTask_MultipleLocalRootsAreBounded) {
    const int limit = DirectorySizeTask::localConcurrencyLimitForRoots(
        {QStringLiteral("/one"), QStringLiteral("/two"), QStringLiteral("/three")});
    EXPECT_GE(limit, 2);
    EXPECT_LE(limit, 4);
}

TEST(DirectorySizeTaskTest, DirectorySizeTask_LocalParallelRootsReportEachDirectoryWhenItFinishes) {
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString slow = QDir(temp.path()).filePath(QStringLiteral("slow"));
    const QString fast = QDir(temp.path()).filePath(QStringLiteral("fast"));
    ASSERT_TRUE(QDir().mkdir(slow));
    ASSERT_TRUE(QDir().mkdir(fast));

    if (DirectorySizeTask::localConcurrencyLimitForRoots({slow, fast}) <= 1)
        GTEST_SKIP() << "this volume is intentionally limited to serial directory sizing";

    for (int i = 0; i < 3000; ++i)
        writePayload(QDir(slow).filePath(QStringLiteral("file_%1.bin").arg(i)));
    writePayload(QDir(fast).filePath(QStringLiteral("done.bin")), 7);

    auto provider = std::shared_ptr<FileProvider>(LocalFileProvider::instance(),
                                                  [](FileProvider *) {});
    DirectorySizeTask task(47, provider, {slow, fast});
    QSignalSpy ready(&task, &DirectorySizeTask::directorySizeReady);
    QSignalSpy finished(&task, &DirectorySizeTask::finished);

    task.start();

    ASSERT_TRUE(ready.wait(4000));
    ASSERT_GE(ready.count(), 1);
    EXPECT_EQ(ready.at(0).at(0).toString(), fast);
    EXPECT_EQ(ready.at(0).at(1).toLongLong(), 7);
    ASSERT_TRUE(finished.wait(4000) || finished.count() > 0);
}

#ifdef Q_OS_WIN
TEST(DirectorySizeTaskTest, Windows_UncRootsStaySerial) {
    EXPECT_EQ(DirectorySizeTask::localConcurrencyLimitForRoots(
                  {QStringLiteral("\\\\server\\share\\one"),
                   QStringLiteral("\\\\server\\share\\two")}),
              1);
}
#endif

TEST(DirectorySizeTaskTest, DirectorySizeTask_ProgressSurvivesApplicationDispatchTargetTeardown) {
    auto provider = std::make_shared<BlockingProvider>();
    DirectorySizeTask task(44, provider, {QStringLiteral("/ready")});
    QSignalSpy progress(&task, &DirectorySizeTask::progress);
    QSignalSpy finished(&task, &DirectorySizeTask::finished);

    task.start();
    ASSERT_TRUE(provider->waitUntilListReturned());
    QThread::msleep(50);

    QCoreApplication::removePostedEvents(QCoreApplication::instance(), QEvent::MetaCall);

    ASSERT_TRUE(finished.wait(4000));
    ASSERT_EQ(progress.count(), 1);
    const QList<QVariant> update = progress.takeFirst();
    EXPECT_EQ(update.at(0).toInt(), 1);
    EXPECT_EQ(update.at(1).toInt(), 1);
    EXPECT_EQ(update.at(2).toLongLong(), 0);
}

TEST(DirectorySizeTaskTest, DirectorySizeTask_DestructionWhileProviderIsBlockedCompletesAfterUnblock) {
    auto provider = std::make_shared<BlockingProvider>();
    const std::weak_ptr<BlockingProvider> providerLifetime = provider;
    auto *task =
        new DirectorySizeTask(45, provider, {QStringLiteral("/first"), QStringLiteral("/second")});
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

#ifdef Q_OS_WIN
TEST(DirectorySizeTaskTest, Windows_LocalProviderTraversesLongPaths) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    QString deep = dir.path();
    for (int i = 0; deep.size() < 285; ++i) {
        deep = QDir(deep).filePath(QStringLiteral("segment_%1_long_name").arg(i));
        createDirectoryWin32(deep);
    }
    const QString payloadPath = QDir(deep).filePath(QStringLiteral("payload.bin"));
    writeFileWin32(payloadPath, QByteArray(321, 'x'));
    ASSERT_GT(payloadPath.size(), 260);

    auto provider = std::shared_ptr<FileProvider>(LocalFileProvider::instance(),
                                                  [](FileProvider *) {});
    DirectorySizeTask task(46, provider, {dir.path()});
    QSignalSpy finished(&task, &DirectorySizeTask::finished);

    task.start();

    ASSERT_TRUE(finished.wait(4000));
    ASSERT_EQ(finished.count(), 1);
    const QList<QVariant> result = finished.takeFirst();
    EXPECT_EQ(result.at(0).toULongLong(), 46u);
    EXPECT_EQ(result.at(1).toLongLong(), 321);
    EXPECT_FALSE(result.at(2).toBool());
}
#endif
