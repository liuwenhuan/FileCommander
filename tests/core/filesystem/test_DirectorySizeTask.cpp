#include <gtest/gtest.h>

#include <QSignalSpy>

#include <condition_variable>
#include <memory>
#include <mutex>

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

    void waitUntilFirstRootEntered() const {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_entered.wait(lock, [this] { return m_firstRootEntered; });
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

} // namespace

TEST(DirectorySizeTask, CancelStopsBeforeTheNextRoot) {
    auto provider = std::make_shared<BlockingProvider>();
    DirectorySizeTask task(42, provider, {QStringLiteral("/first"), QStringLiteral("/second")});
    QSignalSpy finished(&task, &DirectorySizeTask::finished);

    task.start();
    provider->waitUntilFirstRootEntered();
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
