#include "NullMediaEngine.h"

NullMediaEngine::NullMediaEngine(QObject *parent) : MediaEngine(parent) {}

void NullMediaEngine::initialize() {}

void NullMediaEngine::load(const MediaSource &, MediaKind) {
    emit stateChanged(MediaState::Failed);
    emit errorOccurred(QStringLiteral("Media playback is not available in this build."));
}

QWidget *NullMediaEngine::videoSurface() { return nullptr; }
