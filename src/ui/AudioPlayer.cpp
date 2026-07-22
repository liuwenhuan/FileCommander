#include "AudioPlayer.h"

#include <stdexcept>

#include <mpv/client.h>

AudioPlayer::AudioPlayer(QObject *parent) : QObject(parent) {
    m_mpv = mpv_create();
    if (!m_mpv)
        throw std::runtime_error("could not create mpv context");

    // Lean, quiet, audio-only core: no config, no on-screen controls, and no
    // video output at all (vid=no) so an embedded cover-art "video" track is not
    // decoded and no window/VO is ever spawned.
    mpv_set_option_string(m_mpv, "config", "no");
    mpv_set_option_string(m_mpv, "osc", "no");
    mpv_set_option_string(m_mpv, "input-default-bindings", "no");
    mpv_set_option_string(m_mpv, "input-vo-keyboard", "no");
    mpv_set_option_string(m_mpv, "vid", "no");   // audio only
    mpv_set_option_string(m_mpv, "vo", "null");  // never open a video window
    mpv_set_option_string(m_mpv, "force-window", "no");

    if (mpv_initialize(m_mpv) < 0)
        throw std::runtime_error("could not initialize mpv");

    connect(this, &AudioPlayer::mpvEvents, this, &AudioPlayer::onMpvEvents,
            Qt::QueuedConnection);
    mpv_set_wakeup_callback(m_mpv, &AudioPlayer::onWakeup, this);
}

AudioPlayer::~AudioPlayer() {
    if (m_mpv) {
        mpv_set_wakeup_callback(m_mpv, nullptr, nullptr);
        mpv_terminate_destroy(m_mpv);
    }
}

void AudioPlayer::onWakeup(void *ctx) {
    // On mpv's thread: only bounce a queued signal to the GUI thread.
    auto *self = static_cast<AudioPlayer *>(ctx);
    emit self->mpvEvents();
}

void AudioPlayer::onMpvEvents() {
    if (!m_mpv)
        return;
    while (true) {
        mpv_event *ev = mpv_wait_event(m_mpv, 0);
        if (!ev || ev->event_id == MPV_EVENT_NONE)
            break;
        switch (ev->event_id) {
        case MPV_EVENT_START_FILE:
            m_ended = false;
            break;
        case MPV_EVENT_END_FILE: {
            auto *ef = static_cast<mpv_event_end_file *>(ev->data);
            if (ef && ef->reason == MPV_END_FILE_REASON_EOF)
                m_ended = true;
            break;
        }
        default:
            break;
        }
    }
}

void AudioPlayer::load(const QString &path) {
    if (!m_mpv)
        return;
    m_currentPath = path;
    m_ended = false;
    const QByteArray file = path.toUtf8();
    const char *cmd[] = {"loadfile", file.constData(), nullptr};
    mpv_command_async(m_mpv, 0, cmd);
    // Force playback on: a previous clip paused/ended at EOF would otherwise
    // carry its pause state into the new file.
    int unpaused = 0;
    mpv_set_property_async(m_mpv, 0, "pause", MPV_FORMAT_FLAG, &unpaused);
}

void AudioPlayer::stop() {
    if (!m_mpv)
        return;
    const char *cmd[] = {"stop", nullptr};
    mpv_command_async(m_mpv, 0, cmd);
}

void AudioPlayer::playPause() {
    if (!m_mpv)
        return;
    // Finished at EOF: reload to replay from the start (nothing to unpause).
    if (m_ended) {
        if (!m_currentPath.isEmpty())
            load(m_currentPath);
        return;
    }
    const bool isPaused = getFlag("pause");
    int flag = isPaused ? 0 : 1;
    mpv_set_property_async(m_mpv, 0, "pause", MPV_FORMAT_FLAG, &flag);
}

void AudioPlayer::seekFraction(double fraction) {
    if (!m_mpv)
        return;
    const double dur = durationSeconds();
    if (dur <= 0.0)
        return;
    double target = fraction * dur;
    if (target < 0.0)
        target = 0.0;
    mpv_set_property_async(m_mpv, 0, "time-pos", MPV_FORMAT_DOUBLE, &target);
}

void AudioPlayer::setVolume(int volume) {
    if (!m_mpv)
        return;
    double v = volume;
    mpv_set_property_async(m_mpv, 0, "volume", MPV_FORMAT_DOUBLE, &v);
}

void AudioPlayer::setMute(bool mute) {
    if (!m_mpv)
        return;
    int flag = mute ? 1 : 0;
    mpv_set_property_async(m_mpv, 0, "mute", MPV_FORMAT_FLAG, &flag);
}

double AudioPlayer::getDouble(const char *prop) const {
    if (!m_mpv)
        return 0.0;
    double value = 0.0;
    if (mpv_get_property(m_mpv, prop, MPV_FORMAT_DOUBLE, &value) < 0)
        return 0.0;
    return value;
}

bool AudioPlayer::getFlag(const char *prop) const {
    if (!m_mpv)
        return false;
    int flag = 0;
    if (mpv_get_property(m_mpv, prop, MPV_FORMAT_FLAG, &flag) < 0)
        return false;
    return flag != 0;
}

double AudioPlayer::durationSeconds() const { return getDouble("duration"); }
double AudioPlayer::positionSeconds() const { return getDouble("time-pos"); }
bool AudioPlayer::paused() const { return getFlag("pause"); }
bool AudioPlayer::ended() const { return m_ended; }

QString AudioPlayer::metadata(const QString &key) const {
    if (!m_mpv)
        return QString();
    const QByteArray prop = QByteArray("metadata/by-key/") + key.toUtf8();
    char *val = mpv_get_property_string(m_mpv, prop.constData());
    if (!val)
        return QString();
    QString result = QString::fromUtf8(val);
    mpv_free(val);
    return result;
}
