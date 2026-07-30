#include "NullMediaEngine.h"

NullMediaEngine::NullMediaEngine(QObject *parent) : MediaEngine(parent) {}

void NullMediaEngine::initialize() {}

void NullMediaEngine::load(const MediaSource &, MediaKind) {
    m_state = MediaState::Failed;
    emit stateChanged(m_state);
    emit errorOccurred(QStringLiteral("Media playback is not available in this build."));
}

void NullMediaEngine::stop() {
    if (m_state == MediaState::Idle)
        return;
    m_state = MediaState::Idle;
    emit metadataChanged({});
    emit stateChanged(m_state);
}

QWidget *NullMediaEngine::videoSurface() { return nullptr; }

MediaState NullMediaEngine::state() const { return m_state; }
