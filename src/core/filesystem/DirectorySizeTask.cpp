#include "DirectorySizeTask.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QThread>
#include <QtConcurrent/QtConcurrent>

#include <algorithm>
#include <chrono>
#include <future>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#ifdef Q_OS_WIN
#include <windows.h>
#include <winioctl.h>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#elif defined(Q_OS_LINUX)
#include <sys/stat.h>
#include <sys/sysmacros.h>
#endif

#include "FileProvider.h"

namespace {

enum class DriveKind { Remote, Rotational, SolidState, Unknown };

struct RootResult {
    int index = 0;
    QString path;
    qint64 bytes = 0;
    bool cancelled = false;
};

bool looksLikeUncPath(const QString &path) {
    return path.startsWith(QStringLiteral("\\\\")) || path.startsWith(QStringLiteral("//"));
}

void appendProgress(const std::shared_ptr<DirectorySizeTask::State> &state, int completedRoots,
                    int totalRoots, qint64 bytes) {
    std::lock_guard<std::mutex> lock(state->progressMutex);
    state->progressUpdates.append({completedRoots, totalRoots, bytes});
}

void appendRootReady(const std::shared_ptr<DirectorySizeTask::State> &state, const QString &path,
                     qint64 bytes) {
    std::lock_guard<std::mutex> lock(state->progressMutex);
    state->rootReadyUpdates.append({path, bytes});
}

#ifdef Q_OS_WIN
QString volumeRootForPath(const QString &path) {
    if (looksLikeUncPath(path))
        return {};
    QString root = QDir(path).rootPath();
    if (root.size() >= 2 && root.at(1) == QLatin1Char(':'))
        return QDir::toNativeSeparators(root);
    return {};
}

std::optional<bool> windowsVolumeIncursSeekPenalty(const QString &root) {
    if (root.size() < 2 || root.at(1) != QLatin1Char(':'))
        return std::nullopt;

    const QString devicePath =
        QStringLiteral("\\\\.\\") + root.left(2).toUpper();
    const std::wstring wide = devicePath.toStdWString();
    HANDLE handle = CreateFileW(wide.c_str(), FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return std::nullopt;

    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    query.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR descriptor = {};
    DWORD bytesReturned = 0;
    const BOOL ok = DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                                    &descriptor, sizeof(descriptor), &bytesReturned, nullptr);
    CloseHandle(handle);
    if (!ok || bytesReturned < sizeof(descriptor))
        return std::nullopt;
    return descriptor.IncursSeekPenalty != FALSE;
}
#elif defined(Q_OS_LINUX)
std::optional<bool> linuxPathIsRotational(const QString &path) {
    struct stat st;
    const QByteArray encoded = QFile::encodeName(path);
    if (::stat(encoded.constData(), &st) != 0)
        return std::nullopt;

    const QString dev = QStringLiteral("%1:%2")
                            .arg(static_cast<unsigned long>(major(st.st_dev)))
                            .arg(static_cast<unsigned long>(minor(st.st_dev)));
    const QString base = QStringLiteral("/sys/dev/block/") + dev;
    const QStringList candidates = {base + QStringLiteral("/queue/rotational"),
                                    base + QStringLiteral("/../queue/rotational"),
                                    base + QStringLiteral("/../../queue/rotational")};
    for (const QString &candidate : candidates) {
        QFile f(candidate);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        const QByteArray value = f.readAll().trimmed();
        if (value == "0")
            return false;
        if (value == "1")
            return true;
    }
    return std::nullopt;
}
#endif

DriveKind driveKindForPath(const QString &path) {
    if (looksLikeUncPath(path))
        return DriveKind::Remote;

#ifdef Q_OS_WIN
    const QString root = volumeRootForPath(path);
    if (root.isEmpty())
        return DriveKind::Unknown;
    const std::wstring rootWide = root.toStdWString();
    if (GetDriveTypeW(rootWide.c_str()) == DRIVE_REMOTE)
        return DriveKind::Remote;
    const std::optional<bool> seekPenalty = windowsVolumeIncursSeekPenalty(root);
    if (!seekPenalty.has_value())
        return DriveKind::Unknown;
    return seekPenalty.value() ? DriveKind::Rotational : DriveKind::SolidState;
#elif defined(Q_OS_LINUX)
    const std::optional<bool> rotational = linuxPathIsRotational(path);
    if (!rotational.has_value())
        return DriveKind::Unknown;
    return rotational.value() ? DriveKind::Rotational : DriveKind::SolidState;
#else
    Q_UNUSED(path);
    return DriveKind::Unknown;
#endif
}

qint64 rootBytes(const std::shared_ptr<DirectorySizeTask::State> &state, const QString &root,
                 bool *cancelled);

RootResult computeRoot(const std::shared_ptr<DirectorySizeTask::State> &state, int index) {
    bool cancelled = false;
    const QString path = state->roots.at(index);
    const qint64 bytes = rootBytes(state, path, &cancelled);
    if (!cancelled)
        appendRootReady(state, path, bytes);
    return {index, path, bytes, cancelled};
}

DirectorySizeTask::Result runRootsSerial(const std::shared_ptr<DirectorySizeTask::State> &state) {
    DirectorySizeTask::Result result;
    const int totalRoots = state->roots.size();
    for (int i = 0; i < totalRoots; ++i) {
        if (state->cancelled.load(std::memory_order_relaxed)) {
            result.cancelled = true;
            break;
        }

        const RootResult root = computeRoot(state, i);
        if (root.cancelled) {
            result.cancelled = true;
            break;
        }

        result.bytes += root.bytes;
        ++result.completedRoots;
        appendProgress(state, result.completedRoots, totalRoots, result.bytes);
    }
    result.cancelled = result.cancelled || state->cancelled.load(std::memory_order_relaxed);
    return result;
}

DirectorySizeTask::Result runLocalRootsParallel(
    const std::shared_ptr<DirectorySizeTask::State> &state) {
    const int totalRoots = state->roots.size();
    const int concurrency = DirectorySizeTask::localConcurrencyLimitForRoots(state->roots);
    if (concurrency <= 1 || totalRoots <= 1)
        return runRootsSerial(state);

    DirectorySizeTask::Result result;
    std::vector<std::pair<int, std::future<RootResult>>> active;
    int nextToLaunch = 0;

    auto launchMore = [&] {
        while (!state->cancelled.load(std::memory_order_relaxed) && nextToLaunch < totalRoots &&
               static_cast<int>(active.size()) < concurrency) {
            const int index = nextToLaunch++;
            active.emplace_back(index, std::async(std::launch::async, [state, index] {
                                    return computeRoot(state, index);
                                }));
        }
    };

    launchMore();
    while (!active.empty() || nextToLaunch < totalRoots) {
        bool madeProgress = false;
        for (auto it = active.begin(); it != active.end();) {
            if (it->second.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
                ++it;
                continue;
            }
            RootResult root = it->second.get();
            if (root.cancelled) {
                result.cancelled = true;
                state->cancelled.store(true, std::memory_order_relaxed);
            } else {
                result.bytes += root.bytes;
                ++result.completedRoots;
                appendProgress(state, result.completedRoots, totalRoots, result.bytes);
            }
            it = active.erase(it);
            madeProgress = true;
        }
        if (state->cancelled.load(std::memory_order_relaxed) && active.empty()) {
            result.cancelled = true;
            break;
        }
        if (result.cancelled)
            break;

        launchMore();
        if (!madeProgress)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    for (auto &running : active) {
        RootResult root = running.second.get();
        if (root.cancelled)
            result.cancelled = true;
    }
    result.cancelled = result.cancelled || state->cancelled.load(std::memory_order_relaxed);
    return result;
}

} // namespace

qint64 DirectorySizeTask::walkLocalDirectory(const std::shared_ptr<State> &state,
                                             const QString &path, bool *cancelled) {
    qint64 total = 0;
    QDirIterator it(path, QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        if (state->cancelled.load(std::memory_order_relaxed)) {
            *cancelled = true;
            return 0;
        }
        it.next();
        total += it.fileInfo().size();
    }
    return total;
}

int DirectorySizeTask::localConcurrencyLimitForRoots(const QStringList &roots) {
    if (roots.size() <= 1)
        return 1;

    int limit = 4;
    for (const QString &root : roots) {
        switch (driveKindForPath(root)) {
        case DriveKind::Remote:
            return 1;
        case DriveKind::Rotational:
        case DriveKind::Unknown:
            limit = std::min(limit, 2);
            break;
        case DriveKind::SolidState:
            break;
        }
    }
    return qBound(1, std::min(limit, roots.size()), 4);
}

qint64 DirectorySizeTask::walkDirectory(const std::shared_ptr<State> &state, const QString &path,
                                        bool *cancelled) {
    if (state->cancelled.load(std::memory_order_relaxed)) {
        *cancelled = true;
        return 0;
    }

    const QVector<FileInfo> entries = state->provider->list(path, /*showHidden=*/true);
    if (state->cancelled.load(std::memory_order_relaxed)) {
        *cancelled = true;
        return 0;
    }

    qint64 total = 0;
    for (const FileInfo &entry : entries) {
        if (state->cancelled.load(std::memory_order_relaxed)) {
            *cancelled = true;
            return 0;
        }
        if (entry.isParentEntry())
            continue;
        // Never follow a directory symlink. Apart from local loops, a provider
        // cannot promise that a link stays on the same backend or permission set.
        if (entry.isDir() && !entry.isSymLink()) {
            total += DirectorySizeTask::walkDirectory(state, entry.path(), cancelled);
            if (*cancelled)
                return 0;
        } else {
            total += entry.size();
        }
    }
    return total;
}

namespace {

qint64 rootBytes(const std::shared_ptr<DirectorySizeTask::State> &state, const QString &root,
                 bool *cancelled) {
    const auto symlinkSize = state->symlinkRootSizes.constFind(root);
    if (symlinkSize != state->symlinkRootSizes.cend())
        return symlinkSize.value();
    if (state->providerIsLocal)
        return DirectorySizeTask::walkLocalDirectory(state, root, cancelled);
    return DirectorySizeTask::walkDirectory(state, root, cancelled);
}

} // namespace

DirectorySizeTask::DirectorySizeTask(quint64 requestId, std::shared_ptr<FileProvider> provider,
                                     QStringList roots, QObject *parent,
                                     QHash<QString, qint64> symlinkRootSizes)
    : QObject(parent), m_requestId(requestId), m_state(std::make_shared<State>()) {
    m_state->provider = std::move(provider);
    m_state->providerIsLocal = m_state->provider && m_state->provider->isLocalFilesystem();
    m_state->roots = std::move(roots);
    m_state->symlinkRootSizes = std::move(symlinkRootSizes);
    m_progressTimer.setInterval(10);
    connect(&m_progressTimer, &QTimer::timeout, this, &DirectorySizeTask::drainProgress);
    connect(&m_watcher, &QFutureWatcher<Result>::finished, this, [this] {
        m_progressTimer.stop();
        drainProgress();
        const Result result = m_watcher.result();
        emit finished(m_requestId, result.bytes, result.cancelled);
    });
}

DirectorySizeTask::~DirectorySizeTask() {
    cancel();
}

void DirectorySizeTask::start() {
    if (m_started)
        return;
    m_started = true;

    const std::shared_ptr<State> state = m_state;
    m_progressTimer.start();
    m_watcher.setFuture(QtConcurrent::run([state] {
        return state->providerIsLocal ? runLocalRootsParallel(state) : runRootsSerial(state);
    }));
}

void DirectorySizeTask::cancel() {
    m_state->cancelled.store(true, std::memory_order_relaxed);
}

void DirectorySizeTask::drainProgress() {
    QVector<ProgressUpdate> updates;
    QVector<RootReadyUpdate> rootUpdates;
    {
        std::lock_guard<std::mutex> lock(m_state->progressMutex);
        rootUpdates.swap(m_state->rootReadyUpdates);
        updates.swap(m_state->progressUpdates);
    }
    for (const RootReadyUpdate &update : rootUpdates)
        emit directorySizeReady(update.path, update.bytes);
    for (const ProgressUpdate &update : updates) {
        emit progress(update.completedRoots, update.totalRoots, update.bytes);
    }
}
