#include "AnimatedImage.h"

#include <QElapsedTimer>
#include <QImageReader>
#include <QMovie>

#include "theme/Phosphor.h"

struct AnimatedImage::Private {
    QMovie *movie = nullptr;
    qint64 tintedFrames = 0;
    qint64 tintTotalUs = 0;
};

AnimatedImage::AnimatedImage(QObject *parent)
    : QObject(parent), d(std::make_unique<Private>()) {}

AnimatedImage::~AnimatedImage() = default;

bool AnimatedImage::isAnimated(const QString &path) {
    QImageReader reader(path);
    if (!reader.supportsAnimation())
        return false;
    // supportsAnimation() is a property of the FORMAT, not of the file: every
    // GIF answers yes, including the single-frame ones that are just images.
    // Those are better on the still path, which can rotate and zoom them.
    return reader.imageCount() > 1;
}

bool AnimatedImage::play(const QString &path) {
    stop();
    auto movie = std::make_unique<QMovie>(path);
    if (!movie->isValid() || movie->frameCount() == 1)
        return false;

    d->movie = movie.release();
    d->movie->setParent(this);
    d->movie->setCacheMode(QMovie::CacheNone); // a long GIF should not be held whole
    connect(d->movie, &QMovie::frameChanged, this, [this](int) { onFrameChanged(); });
    d->movie->start();
    // QMovie emits frameChanged for frames after the first, so the first one is
    // delivered here or the view stays empty until the animation loops.
    onFrameChanged();
    return true;
}

void AnimatedImage::stop() {
    if (!d->movie)
        return;
    d->movie->stop();
    d->movie->deleteLater();
    d->movie = nullptr;
    d->tintedFrames = 0;
    d->tintTotalUs = 0;
}

void AnimatedImage::setPaused(bool paused) {
    if (d->movie)
        d->movie->setPaused(paused);
}

bool AnimatedImage::isPaused() const {
    return d->movie && d->movie->state() == QMovie::Paused;
}

bool AnimatedImage::isPlaying() const {
    return d->movie && d->movie->state() == QMovie::Running;
}

QSize AnimatedImage::frameSize() const {
    return d->movie ? d->movie->currentImage().size() : QSize();
}

int AnimatedImage::frameCount() const {
    return d->movie ? d->movie->frameCount() : 0;
}

qint64 AnimatedImage::frameCostUs() const {
    return d->tintedFrames > 0 ? d->tintTotalUs / d->tintedFrames : 0;
}

void AnimatedImage::onFrameChanged() {
    if (!d->movie)
        return;
    QImage frame = d->movie->currentImage();
    if (frame.isNull())
        return;

    const QColor tint = fc::previewTint();
    if (tint.isValid()) {
        QElapsedTimer timer;
        timer.start();
        if (frame.format() != QImage::Format_ARGB32)
            frame = frame.convertToFormat(QImage::Format_ARGB32);
        fc::tintImage(frame, tint);
        fc::applyScanlines(frame);
        d->tintTotalUs += timer.nsecsElapsed() / 1000;
        ++d->tintedFrames;
    }
    emit frameReady(frame);
}
