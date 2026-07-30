#include "ImagePreviewLoader.h"

#include "FileInfo.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QImageWriter>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>
#include <QThreadPool>
#include <QVector>
#include <QWaitCondition>
#include <QtConcurrent>

#include <atomic>
#include <optional>
#include <utility>

namespace {

struct LoadRequest {
    quint64 generation = 0;
    QString path;
};

struct RenderRequest {
    quint64 generation = 0;
    QImage source;
    QSize target;
    QTransform transform;
};

class PreviewWorkerPool : public QThreadPool {
public:
    PreviewWorkerPool() {
        setMaxThreadCount(1);
        setExpiryTimeout(-1);
    }
};

void wakeFileAccessWaiters();

class PreviewWorkerService {
public:
    PreviewWorkerService() {
        if (auto *application = QCoreApplication::instance()) {
            QObject::connect(application, &QCoreApplication::aboutToQuit, application, [this] {
                m_shuttingDown.store(true, std::memory_order_release);
                wakeFileAccessWaiters();
            });
        }
    }

    bool shuttingDown() const {
        return m_shuttingDown.load(std::memory_order_acquire);
    }

    PreviewWorkerPool loadPool;
    PreviewWorkerPool renderPool;
    PreviewWorkerPool rotationPool;

private:
    std::atomic_bool m_shuttingDown{false};
};

PreviewWorkerService *workerService() {
    // Deliberately leaked like ThumbnailCache: active workers may survive their
    // widgets, so pool destruction must never run after QApplication teardown.
    static auto *service = new PreviewWorkerService;
    return service;
}

QThreadPool *loadPool() {
    return &workerService()->loadPool;
}

QThreadPool *renderPool() {
    return &workerService()->renderPool;
}

QThreadPool *rotationPool() {
    return &workerService()->rotationPool;
}

struct FileAccessEntry {
    QMutex mutex;
    QWaitCondition condition;
    int activeReaders = 0;
    int pendingWriters = 0;
    bool writerActive = false;
};

struct FileAccessRegistry {
    QMutex mutex;
    QHash<QString, std::weak_ptr<FileAccessEntry>> entries;
};

FileAccessRegistry *fileAccessRegistry() {
    // Workers can still hold entries while the process exits. Keep the
    // registry alive with the deliberately leaked worker service.
    static auto *registry = new FileAccessRegistry;
    return registry;
}

std::shared_ptr<FileAccessEntry> fileAccessEntry(const QString &path) {
    QString key = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
#ifdef Q_OS_WIN
    key = key.toCaseFolded();
#endif
    FileAccessRegistry *registry = fileAccessRegistry();
    QMutexLocker locker(&registry->mutex);
    if (auto existing = registry->entries.value(key).lock())
        return existing;
    auto entry = std::make_shared<FileAccessEntry>();
    registry->entries.insert(key, entry);
    return entry;
}

void wakeFileAccessWaiters() {
    QVector<std::shared_ptr<FileAccessEntry>> entries;
    {
        FileAccessRegistry *registry = fileAccessRegistry();
        QMutexLocker locker(&registry->mutex);
        auto it = registry->entries.begin();
        while (it != registry->entries.end()) {
            if (auto entry = it.value().lock()) {
                entries.append(std::move(entry));
                ++it;
            } else {
                it = registry->entries.erase(it);
            }
        }
    }
    for (const auto &entry : entries) {
        QMutexLocker locker(&entry->mutex);
        entry->condition.wakeAll();
    }
}

class FileReadLease {
public:
    FileReadLease(const QString &path, std::function<bool()> cancelled,
                  std::function<void()> waiting)
        : m_entry(fileAccessEntry(path)) {
        QMutexLocker locker(&m_entry->mutex);
        bool announcedWait = false;
        while (m_entry->pendingWriters > 0 || m_entry->writerActive) {
            if (cancelled())
                return;
            if (!announcedWait) {
                announcedWait = true;
                locker.unlock();
                waiting();
                locker.relock();
                continue;
            }
            m_entry->condition.wait(&m_entry->mutex);
        }
        if (cancelled())
            return;
        ++m_entry->activeReaders;
        m_acquired = true;
    }

    ~FileReadLease() {
        if (!m_acquired)
            return;
        QMutexLocker locker(&m_entry->mutex);
        --m_entry->activeReaders;
        m_entry->condition.wakeAll();
    }

    explicit operator bool() const { return m_acquired; }

private:
    std::shared_ptr<FileAccessEntry> m_entry;
    bool m_acquired = false;
};

class FileWriteReservation {
public:
    explicit FileWriteReservation(const QString &path) : m_entry(fileAccessEntry(path)) {
        QMutexLocker locker(&m_entry->mutex);
        ++m_entry->pendingWriters;
    }

    ~FileWriteReservation() {
        if (!m_finished)
            finish();
    }

    bool begin(const std::function<bool()> &cancelled) {
        QMutexLocker locker(&m_entry->mutex);
        while (m_entry->activeReaders > 0 || m_entry->writerActive) {
            if (cancelled())
                return false;
            m_entry->condition.wait(&m_entry->mutex);
        }
        if (cancelled())
            return false;
        m_entry->writerActive = true;
        m_begun = true;
        return true;
    }

    void finish() {
        if (m_finished)
            return;
        QMutexLocker locker(&m_entry->mutex);
        if (m_begun)
            m_entry->writerActive = false;
        --m_entry->pendingWriters;
        m_finished = true;
        m_entry->condition.wakeAll();
    }

private:
    std::shared_ptr<FileAccessEntry> m_entry;
    bool m_begun = false;
    bool m_finished = false;
};

bool persistImageRotation(const QString &path, int degrees) {
    const QFileInfo fileInfo(path);
    const QString suffix = FileInfo::suffixForName(fileInfo.fileName()).toLower();
    const int rotation = ((degrees % 360) + 360) % 360;
    QTransform transform;
    transform.rotate(degrees);
    bool saved = false;

    if (suffix == QLatin1String("jpg") || suffix == QLatin1String("jpeg")) {
        const QString jpegtran = QStandardPaths::findExecutable(QStringLiteral("jpegtran"));
        if (!jpegtran.isEmpty()) {
            const QString temporary =
                fileInfo.absoluteDir().filePath(QStringLiteral(".%1.rot.tmp")
                                                    .arg(fileInfo.fileName()));
            QProcess process;
            process.start(jpegtran, {QStringLiteral("-rotate"), QString::number(rotation),
                                     QStringLiteral("-copy"), QStringLiteral("all"),
                                     QStringLiteral("-outfile"), temporary, path});
            if (process.waitForFinished(15000) &&
                process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0 &&
                QFileInfo::exists(temporary)) {
                if (QFile::remove(path) && QFile::rename(temporary, path))
                    saved = true;
                else
                    QFile::remove(temporary);
            } else {
                QFile::remove(temporary);
            }
        }
    }

    if (!saved) {
        QImageReader reader(path);
        QImage image = reader.read();
        if (!image.isNull()) {
            image = image.transformed(transform, Qt::FastTransformation);
            QImageWriter writer(path);
            if (suffix == QLatin1String("jpg") || suffix == QLatin1String("jpeg"))
                writer.setQuality(100);
            saved = writer.write(image);
        }
    }
    return saved;
}

} // namespace

struct ImagePreviewLoaderState {
    QMutex mutex;
    QWaitCondition idleCondition;
    std::atomic<quint64> cancelBeforeGeneration{0};
    std::atomic_bool destroyed{false};
    bool loadActive = false;
    bool renderActive = false;
    int activeRotations = 0;
    int pendingDeliveries = 0;
    quint64 nextRotationSequence = 0;
    std::optional<LoadRequest> pendingLoad;
    std::optional<RenderRequest> pendingRender;
    std::function<void(ImagePreviewLoader::WorkerCheckpoint, quint64)> checkpoint;
};

struct ImagePreviewLoaderDeliveryToken {
    QPointer<ImagePreviewLoader> target;
};

namespace {

bool isCancelled(const std::shared_ptr<ImagePreviewLoaderState> &state, quint64 generation) {
    return workerService()->shuttingDown() ||
           state->destroyed.load(std::memory_order_acquire) ||
           generation < state->cancelBeforeGeneration.load(std::memory_order_acquire);
}

void runCheckpoint(const std::shared_ptr<ImagePreviewLoaderState> &state,
                   ImagePreviewLoader::WorkerCheckpoint checkpoint, quint64 generation) {
    std::function<void(ImagePreviewLoader::WorkerCheckpoint, quint64)> hook;
    {
        QMutexLocker locker(&state->mutex);
        hook = state->checkpoint;
    }
    if (hook)
        hook(checkpoint, generation);
}

void beginDelivery(const std::shared_ptr<ImagePreviewLoaderState> &state) {
    QMutexLocker locker(&state->mutex);
    ++state->pendingDeliveries;
}

void finishDelivery(const std::shared_ptr<ImagePreviewLoaderState> &state) {
    QMutexLocker locker(&state->mutex);
    --state->pendingDeliveries;
    state->idleCondition.wakeAll();
}

void postLoaded(const std::shared_ptr<ImagePreviewLoaderState> &state,
                const std::shared_ptr<ImagePreviewLoaderDeliveryToken> &token,
                quint64 generation, QImage image, ImageMetadata metadata, QString error) {
    if (workerService()->shuttingDown())
        return;
    beginDelivery(state);
    QObject *dispatcher = QCoreApplication::instance();
    if (!dispatcher) {
        finishDelivery(state);
        return;
    }
    const bool queued = QMetaObject::invokeMethod(
        dispatcher,
        [state, token, generation, image = std::move(image), metadata = std::move(metadata),
         error = std::move(error)]() mutable {
            if (!isCancelled(state, generation)) {
                if (ImagePreviewLoader *target = token->target.data())
                    emit target->loaded(generation, std::move(image), std::move(metadata),
                                        std::move(error));
            }
            finishDelivery(state);
        },
        Qt::QueuedConnection);
    if (!queued)
        finishDelivery(state);
}

void postRendered(const std::shared_ptr<ImagePreviewLoaderState> &state,
                  const std::shared_ptr<ImagePreviewLoaderDeliveryToken> &token,
                  quint64 generation, QImage image) {
    if (workerService()->shuttingDown())
        return;
    beginDelivery(state);
    QObject *dispatcher = QCoreApplication::instance();
    if (!dispatcher) {
        finishDelivery(state);
        return;
    }
    const bool queued = QMetaObject::invokeMethod(
        dispatcher,
        [state, token, generation, image = std::move(image)]() mutable {
            if (!isCancelled(state, generation)) {
                if (ImagePreviewLoader *target = token->target.data())
                    emit target->rendered(generation, std::move(image));
            }
            finishDelivery(state);
        },
        Qt::QueuedConnection);
    if (!queued)
        finishDelivery(state);
}

void postRotationPersisted(
    const std::shared_ptr<ImagePreviewLoaderState> &state,
    const std::shared_ptr<ImagePreviewLoaderDeliveryToken> &token, QString path, bool saved) {
    if (workerService()->shuttingDown())
        return;
    beginDelivery(state);
    QObject *dispatcher = QCoreApplication::instance();
    if (!dispatcher) {
        finishDelivery(state);
        return;
    }
    const bool queued = QMetaObject::invokeMethod(
        dispatcher,
        [state, token, path = std::move(path), saved]() mutable {
            if (!state->destroyed.load(std::memory_order_acquire)) {
                if (ImagePreviewLoader *target = token->target.data())
                    emit target->rotationPersisted(std::move(path), saved);
            }
            finishDelivery(state);
        },
        Qt::QueuedConnection);
    if (!queued)
        finishDelivery(state);
}

void finishLoadRequest(const std::shared_ptr<ImagePreviewLoaderState> &state,
                       std::optional<LoadRequest> *next) {
    QMutexLocker locker(&state->mutex);
    if (state->pendingLoad) {
        *next = std::move(state->pendingLoad);
        state->pendingLoad.reset();
        return;
    }
    state->loadActive = false;
    state->idleCondition.wakeAll();
}

void runLoadLane(std::shared_ptr<ImagePreviewLoaderState> state,
                 std::shared_ptr<ImagePreviewLoaderDeliveryToken> token, LoadRequest request) {
    std::optional<LoadRequest> current(std::move(request));
    while (current) {
        const quint64 generation = current->generation;
        runCheckpoint(state, ImagePreviewLoader::WorkerCheckpoint::LoadBeforeDecode, generation);
        if (!isCancelled(state, generation)) {
            FileReadLease lease(
                current->path, [state, generation] { return isCancelled(state, generation); },
                [state, generation] {
                    runCheckpoint(state,
                                  ImagePreviewLoader::WorkerCheckpoint::LoadWaitingForFile,
                                  generation);
                });
            if (!lease) {
                current.reset();
                finishLoadRequest(state, &current);
                continue;
            }
            QImageReader reader(current->path);
            ImageMetadata metadata;
            metadata.format = QString::fromLatin1(reader.format()).toUpper();
            metadata.dimensions = reader.size();
            QImage image = reader.read();
            runCheckpoint(state, ImagePreviewLoader::WorkerCheckpoint::LoadAfterDecode,
                          generation);
            if (!isCancelled(state, generation)) {
                QString error;
                if (image.isNull()) {
                    error = reader.errorString();
                } else {
                    if (!metadata.dimensions.isValid())
                        metadata.dimensions = image.size();
                    metadata.depth = image.depth();
                }
                postLoaded(state, token, generation, std::move(image), std::move(metadata),
                           std::move(error));
            }
        }

        current.reset();
        finishLoadRequest(state, &current);
    }
}

void finishRenderRequest(const std::shared_ptr<ImagePreviewLoaderState> &state,
                         std::optional<RenderRequest> *next) {
    QMutexLocker locker(&state->mutex);
    if (state->pendingRender) {
        *next = std::move(state->pendingRender);
        state->pendingRender.reset();
        return;
    }
    state->renderActive = false;
    state->idleCondition.wakeAll();
}

void runRenderLane(std::shared_ptr<ImagePreviewLoaderState> state,
                   std::shared_ptr<ImagePreviewLoaderDeliveryToken> token,
                   RenderRequest request) {
    std::optional<RenderRequest> current(std::move(request));
    while (current) {
        const quint64 generation = current->generation;
        runCheckpoint(state, ImagePreviewLoader::WorkerCheckpoint::RenderBeforeTransform,
                      generation);
        if (!isCancelled(state, generation) && !current->source.isNull() &&
            current->target.width() > 0 && current->target.height() > 0) {
            QImage transformed =
                current->transform.isIdentity()
                    ? current->source
                    : current->source.transformed(current->transform, Qt::SmoothTransformation);
            runCheckpoint(state, ImagePreviewLoader::WorkerCheckpoint::RenderAfterTransform,
                          generation);
            if (!isCancelled(state, generation) && !transformed.isNull()) {
                QImage image = transformed.scaled(current->target, Qt::KeepAspectRatio,
                                                   Qt::SmoothTransformation);
                runCheckpoint(state, ImagePreviewLoader::WorkerCheckpoint::RenderAfterScale,
                              generation);
                if (!isCancelled(state, generation))
                    postRendered(state, token, generation, std::move(image));
            }
        }

        current.reset();
        finishRenderRequest(state, &current);
    }
}

bool isIdle(const ImagePreviewLoaderState &state) {
    return !state.loadActive && !state.renderActive && !state.pendingLoad &&
           !state.pendingRender && state.activeRotations == 0 &&
           state.pendingDeliveries == 0;
}

} // namespace

ImagePreviewLoader::ImagePreviewLoader(QObject *parent)
    : QObject(parent), m_state(std::make_shared<ImagePreviewLoaderState>()),
      m_deliveryToken(std::make_shared<ImagePreviewLoaderDeliveryToken>()) {
    qRegisterMetaType<ImageMetadata>();
    m_deliveryToken->target = this;
}

ImagePreviewLoader::~ImagePreviewLoader() {
    m_deliveryToken->target.clear();
    m_state->destroyed.store(true, std::memory_order_release);
    wakeFileAccessWaiters();
    QMutexLocker locker(&m_state->mutex);
    m_state->pendingLoad.reset();
    m_state->pendingRender.reset();
    m_state->idleCondition.wakeAll();
}

quint64 ImagePreviewLoader::nextGeneration() {
    const quint64 generation = ++m_nextGeneration;
    cancelBefore(generation);
    return generation;
}

quint64 ImagePreviewLoader::load(const QString &path) {
    LoadRequest request{nextGeneration(), path};
    const quint64 generation = request.generation;
    if (workerService()->shuttingDown())
        return generation;
    bool startWorker = false;
    {
        QMutexLocker locker(&m_state->mutex);
        if (m_state->loadActive) {
            m_state->pendingLoad = std::move(request);
        } else {
            m_state->loadActive = true;
            startWorker = true;
        }
    }
    if (startWorker) {
        QtConcurrent::run(loadPool(), [state = m_state, token = m_deliveryToken,
                                       request = std::move(request)]() mutable {
            runLoadLane(std::move(state), std::move(token), std::move(request));
        });
    }
    return generation;
}

quint64 ImagePreviewLoader::render(const QImage &source, QSize target, QTransform transform) {
    RenderRequest request{nextGeneration(), source, target, transform};
    const quint64 generation = request.generation;
    if (workerService()->shuttingDown())
        return generation;
    bool startWorker = false;
    {
        QMutexLocker locker(&m_state->mutex);
        if (m_state->renderActive) {
            m_state->pendingRender = std::move(request);
        } else {
            m_state->renderActive = true;
            startWorker = true;
        }
    }
    if (startWorker) {
        QtConcurrent::run(renderPool(), [state = m_state, token = m_deliveryToken,
                                         request = std::move(request)]() mutable {
            runRenderLane(std::move(state), std::move(token), std::move(request));
        });
    }
    return generation;
}

void ImagePreviewLoader::persistRotation(const QString &path, int degrees) {
    if (workerService()->shuttingDown())
        return;
    auto reservation = std::make_shared<FileWriteReservation>(path);
    quint64 sequence = 0;
    {
        QMutexLocker locker(&m_state->mutex);
        ++m_state->activeRotations;
        sequence = ++m_state->nextRotationSequence;
    }
    QtConcurrent::run(rotationPool(),
                      [state = m_state, token = m_deliveryToken, reservation = std::move(reservation),
                       path, degrees, sequence]() mutable {
                          if (!reservation->begin(
                                  [] { return workerService()->shuttingDown(); })) {
                              reservation->finish();
                              QMutexLocker locker(&state->mutex);
                              --state->activeRotations;
                              state->idleCondition.wakeAll();
                              return;
                          }
                          runCheckpoint(state, WorkerCheckpoint::RotationBeforeWrite, sequence);
                          const bool saved = !workerService()->shuttingDown() &&
                                             persistImageRotation(path, degrees);
                          runCheckpoint(state, WorkerCheckpoint::RotationAfterWrite, sequence);
                          reservation->finish();
                          if (!workerService()->shuttingDown())
                              postRotationPersisted(state, token, path, saved);
                          QMutexLocker locker(&state->mutex);
                          --state->activeRotations;
                          state->idleCondition.wakeAll();
                      });
}

void ImagePreviewLoader::cancelBefore(quint64 generation) {
    quint64 floor = m_state->cancelBeforeGeneration.load(std::memory_order_relaxed);
    while (floor < generation &&
           !m_state->cancelBeforeGeneration.compare_exchange_weak(
               floor, generation, std::memory_order_release, std::memory_order_relaxed)) {
    }
    wakeFileAccessWaiters();
    if (generation > m_nextGeneration)
        m_nextGeneration = generation - 1;

    QMutexLocker locker(&m_state->mutex);
    if (m_state->pendingLoad && m_state->pendingLoad->generation < generation)
        m_state->pendingLoad.reset();
    if (m_state->pendingRender && m_state->pendingRender->generation < generation)
        m_state->pendingRender.reset();
}

void ImagePreviewLoader::setWorkerCheckpointForTest(
    std::function<void(WorkerCheckpoint, quint64)> checkpoint) {
    QMutexLocker locker(&m_state->mutex);
    m_state->checkpoint = std::move(checkpoint);
}

bool ImagePreviewLoader::waitForIdleForTest(int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        {
            QMutexLocker locker(&m_state->mutex);
            if (isIdle(*m_state))
                return true;
        }
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    QMutexLocker locker(&m_state->mutex);
    return isIdle(*m_state);
}

bool ImagePreviewLoader::waitForAllForTest(int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    if (!loadPool()->waitForDone(timeoutMs))
        return false;
    int remaining = timeoutMs - static_cast<int>(timer.elapsed());
    if (remaining <= 0 || !renderPool()->waitForDone(remaining))
        return false;
    remaining = timeoutMs - static_cast<int>(timer.elapsed());
    return remaining > 0 && rotationPool()->waitForDone(remaining);
}
