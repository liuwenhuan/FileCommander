#include "ImagePreviewLoader.h"

#include <QFutureWatcher>
#include <QImageReader>
#include <QtConcurrent>

namespace {

struct LoadResult {
    QImage image;
    ImageMetadata metadata;
    QString error;
};

LoadResult decodeImage(const QString &path) {
    QImageReader reader(path);
    LoadResult result;
    result.metadata.format = QString::fromLatin1(reader.format()).toUpper();
    result.metadata.dimensions = reader.size();
    result.image = reader.read();
    if (result.image.isNull()) {
        result.error = reader.errorString();
        return result;
    }

    if (!result.metadata.dimensions.isValid())
        result.metadata.dimensions = result.image.size();
    result.metadata.depth = result.image.depth();
    return result;
}

QImage renderImage(const QImage &source, const QSize &target, const QTransform &transform) {
    if (source.isNull() || target.width() <= 0 || target.height() <= 0)
        return {};

    const QImage transformed =
        transform.isIdentity() ? source : source.transformed(transform, Qt::SmoothTransformation);
    if (transformed.isNull())
        return {};
    return transformed.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

} // namespace

ImagePreviewLoader::ImagePreviewLoader(QObject *parent) : QObject(parent) {
    qRegisterMetaType<ImageMetadata>();
}

quint64 ImagePreviewLoader::nextGeneration() {
    const quint64 generation = ++m_nextGeneration;
    cancelBefore(generation);
    return generation;
}

quint64 ImagePreviewLoader::load(const QString &path) {
    const quint64 generation = nextGeneration();
    auto *watcher = new QFutureWatcher<LoadResult>(this);
    connect(watcher, &QFutureWatcher<LoadResult>::finished, this, [this, watcher, generation]() {
        const LoadResult result = watcher->result();
        watcher->deleteLater();
        if (generation < m_cancelBeforeGeneration)
            return;
        emit loaded(generation, result.image, result.metadata, result.error);
    });
    watcher->setFuture(QtConcurrent::run([path]() { return decodeImage(path); }));
    return generation;
}

quint64 ImagePreviewLoader::render(const QImage &source, QSize target, QTransform transform) {
    const quint64 generation = nextGeneration();
    auto *watcher = new QFutureWatcher<QImage>(this);
    connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, generation]() {
        const QImage image = watcher->result();
        watcher->deleteLater();
        if (generation < m_cancelBeforeGeneration)
            return;
        emit rendered(generation, image);
    });
    watcher->setFuture(QtConcurrent::run(
        [source, target, transform]() { return renderImage(source, target, transform); }));
    return generation;
}

void ImagePreviewLoader::cancelBefore(quint64 generation) {
    m_cancelBeforeGeneration = qMax(m_cancelBeforeGeneration, generation);
    if (generation > m_nextGeneration)
        m_nextGeneration = generation - 1;
}
