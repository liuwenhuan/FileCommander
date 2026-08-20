#include "MpvMediaEngine.h"

#include "MpvStreamSource.h"
#include "MpvVideoSurface.h"
#include "theme/Phosphor.h"

#include <QDebug>
#include <QMetaObject>
#include <QTimer>

#include <cmath>
#include <stdexcept>

#include <mpv/client.h>

namespace {

constexpr int kPollIntervalMs = 100;

bool differs(double left, double right) {
    return std::abs(left - right) > 0.001;
}

} // namespace

MpvMediaEngine::MpvMediaEngine(QObject *parent) : MpvMediaEngine(Options{}, parent) {}

MpvMediaEngine::MpvMediaEngine(Options options, QObject *parent)
    : MediaEngine(parent), m_options(std::move(options)),
      m_surface(new MpvVideoSurface(*this)) {
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(kPollIntervalMs);
    connect(m_pollTimer, &QTimer::timeout, this, &MpvMediaEngine::pollProperties);
}

MpvMediaEngine::~MpvMediaEngine() {
    if (m_pollTimer)
        m_pollTimer->stop();
    delete m_surface;
    if (m_mpv) {
        mpv_set_wakeup_callback(m_mpv, nullptr, nullptr);
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
    }
}

void MpvMediaEngine::initialize() {
    if (m_initialized)
        return;

    const auto create = m_options.createContext
                            ? m_options.createContext
                            : std::function<mpv_handle *()>([]() { return mpv_create(); });
    const auto initialize =
        m_options.initializeContext
            ? m_options.initializeContext
            : std::function<int(mpv_handle *)>([](mpv_handle *handle) {
                  return mpv_initialize(handle);
              });

    m_mpv = create();
    if (!m_mpv)
        throw std::runtime_error("could not create mpv context");

    mpv_set_option_string(m_mpv, "config", "no");
    mpv_set_option_string(m_mpv, "osc", "no");
    mpv_set_option_string(m_mpv, "input-default-bindings", "no");
    mpv_set_option_string(m_mpv, "input-vo-keyboard", "no");
    mpv_set_option_string(m_mpv, "force-window", "no");
    mpv_set_option_string(m_mpv, "mute", "no");
    mpv_set_option_string(m_mpv, "vo", m_options.headless ? "null" : "libmpv");
    if (m_options.headless)
        mpv_set_option_string(m_mpv, "ao", "null");
    mpv_set_option_string(m_mpv, "cache", "yes");
    mpv_set_option_string(m_mpv, "demuxer-max-bytes", "8MiB");

    if (initialize(m_mpv) < 0) {
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
        throw std::runtime_error("could not initialize mpv");
    }

    if (!MpvStreamSource::registerProtocol(m_mpv))
        qWarning("MpvMediaEngine: libmpv refused fcstream; remote video will download");
    mpv_set_wakeup_callback(m_mpv, &MpvMediaEngine::onWakeup, this);
    m_initialized = true;
    m_pollTimer->start();
}

void MpvMediaEngine::load(const MediaSource &source, MediaKind kind) {
    initialize();
    m_source = source;
    m_kind = kind;
    m_pendingVideoLoad = false;
    m_pendingSeek = -1.0;
    clearObservedValues();

    m_videoMode = kind == MediaKind::Audio ? QStringLiteral("no") : QStringLiteral("auto");
    mpv_set_property_string(m_mpv, "vid", m_videoMode.toUtf8().constData());
    setState(MediaState::Loading);

    if (kind == MediaKind::Video && !m_options.headless && !m_surfaceReady) {
        m_pendingVideoLoad = true;
        return;
    }
    issueLoad();
}

void MpvMediaEngine::issueLoad() {
    if (!m_mpv || m_source.path.isEmpty())
        return;
    const QByteArray path = m_source.path.toUtf8();
    const char *command[] = {"loadfile", path.constData(), nullptr};
    mpv_command_async(m_mpv, 0, command);
    int unpaused = 0;
    mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &unpaused);
    double effectiveSpeed = m_kind == MediaKind::Audio ? 1.0 : m_speed;
    mpv_set_property(m_mpv, "speed", MPV_FORMAT_DOUBLE, &effectiveSpeed);
}

void MpvMediaEngine::stop() {
    m_pendingVideoLoad = false;
    if (m_mpv) {
        const char *command[] = {"stop", nullptr};
        mpv_command_async(m_mpv, 0, command);
    }
    m_source = {};
    clearObservedValues();
    setState(MediaState::Idle);
}

void MpvMediaEngine::playPause() {
    if (!m_mpv || m_source.path.isEmpty())
        return;
    if (ended()) {
        const MediaSource source = m_source;
        const MediaKind kind = m_kind;
        load(source, kind);
        return;
    }
    int pausedFlag = paused() ? 0 : 1;
    if (mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &pausedFlag) >= 0)
        setState(pausedFlag ? MediaState::Paused : MediaState::Playing);
}

void MpvMediaEngine::seekFraction(double fraction) {
    if (!m_mpv)
        return;
    const double duration = durationSeconds();
    if (duration <= 0.0)
        return;
    const double position = qBound(0.0, fraction, 1.0) * duration;
    // At EOF mpv has unloaded the file, so "time-pos" no longer exists and the
    // seek is dropped -- the slider moved and nothing happened. Load it again
    // (the way playPause() replays) and seek once the file is back.
    if (ended()) {
        const MediaSource source = m_source;
        load(source, m_kind);
        m_pendingSeek = position; // after load(), which clears a stale one
        return;
    }
    applySeek(position);
}

void MpvMediaEngine::applySeek(double position) {
    if (mpv_set_property(m_mpv, "time-pos", MPV_FORMAT_DOUBLE, &position) >= 0) {
        m_position = position;
        emit positionChanged(m_position);
    }
}

void MpvMediaEngine::setVolume(int volume) {
    m_volume = qBound(0, volume, 100);
    if (!m_mpv)
        return;
    double value = m_volume;
    mpv_set_property(m_mpv, "volume", MPV_FORMAT_DOUBLE, &value);
}

void MpvMediaEngine::setMute(bool mute) {
    m_muted = mute;
    if (!m_mpv)
        return;
    int value = mute ? 1 : 0;
    mpv_set_property(m_mpv, "mute", MPV_FORMAT_FLAG, &value);
}

void MpvMediaEngine::setSpeed(double speed) {
    m_speed = speed > 0.0 ? speed : 1.0;
    if (m_mpv)
        mpv_set_property(m_mpv, "speed", MPV_FORMAT_DOUBLE, &m_speed);
}

void MpvMediaEngine::setVideoEffect(const VideoEffectSettings &settings) {
    if (m_videoEffect.tint == settings.tint && m_videoEffect.pixelBlock == settings.pixelBlock)
        return;
    m_videoEffect = settings;
    const QString filter = fc::mpvFilterFor(settings.tint, settings.pixelBlock, 0);
    if (m_videoFilter == filter)
        return;
    m_videoFilter = filter;
    if (m_mpv)
        mpv_set_property_string(m_mpv, "vf", filter.toUtf8().constData());
}

void MpvMediaEngine::setVideoRotation(int degrees) {
    // mpv rotates in its own video chain, so this costs nothing per frame --
    // unlike the Windows backend, which paints the frames itself.
    //
    // NOT VERIFIED ON A RUNNING mpv: this backend only builds on Linux and the
    // work was done on Windows. The property is long-standing mpv API and the
    // call is the same shape as the ones above it.
    m_rotation = ((degrees % 360) + 360) % 360;
    if (m_mpv)
        mpv_set_property_string(m_mpv, "video-rotate",
                                QByteArray::number(m_rotation).constData());
}

QWidget *MpvMediaEngine::videoSurface() { return m_surface; }

MediaState MpvMediaEngine::state() const { return m_state; }
MediaKind MpvMediaEngine::currentKind() const { return m_kind; }
MediaSource MpvMediaEngine::currentSource() const { return m_source; }
double MpvMediaEngine::durationSeconds() const { return m_duration; }
double MpvMediaEngine::positionSeconds() const { return m_position; }
bool MpvMediaEngine::paused() const { return m_state == MediaState::Paused; }
bool MpvMediaEngine::ended() const { return m_state == MediaState::Ended; }
int MpvMediaEngine::volume() const { return m_volume; }
bool MpvMediaEngine::muted() const { return m_muted; }
QHash<QString, QString> MpvMediaEngine::metadata() const { return m_metadata; }
QString MpvMediaEngine::metadataValue(const QString &key) const {
    return m_metadata.value(key.toLower());
}
QSize MpvMediaEngine::currentVideoSize() const { return m_videoSize; }
QString MpvMediaEngine::videoCodec() const { return m_videoCodec; }
QString MpvMediaEngine::videoMode() const { return m_videoMode; }
double MpvMediaEngine::playbackSpeed() const { return getDouble("speed"); }

void MpvMediaEngine::onWakeup(void *context) {
    auto *engine = static_cast<MpvMediaEngine *>(context);
    QMetaObject::invokeMethod(engine, "processEvents", Qt::QueuedConnection);
}

void MpvMediaEngine::processEvents() {
    if (!m_mpv)
        return;
    while (true) {
        mpv_event *event = mpv_wait_event(m_mpv, 0);
        if (!event || event->event_id == MPV_EVENT_NONE)
            break;
        switch (event->event_id) {
        case MPV_EVENT_START_FILE:
            setState(MediaState::Loading);
            break;
        case MPV_EVENT_FILE_LOADED:
            pollProperties();
            refreshMetadata();
            setState(getFlag("pause") ? MediaState::Paused : MediaState::Playing);
            if (m_pendingSeek >= 0.0) {
                const double position = m_pendingSeek;
                m_pendingSeek = -1.0;
                applySeek(position); // a seek that arrived while the file was at EOF
            }
            break;
        case MPV_EVENT_END_FILE: {
            auto *end = static_cast<mpv_event_end_file *>(event->data);
            if (end && end->reason == MPV_END_FILE_REASON_EOF) {
                setState(MediaState::Ended);
            } else if (end && end->reason == MPV_END_FILE_REASON_ERROR) {
                setState(MediaState::Failed);
                emit errorOccurred(
                    QStringLiteral("Could not play media: %1").arg(m_source.path));
            }
            break;
        }
        default:
            break;
        }
    }
}

void MpvMediaEngine::pollProperties() {
    // Ended is polled no further: mpv has unloaded the file, so every property
    // reads back 0 and the poll would wipe the duration and position the UI
    // still needs -- a zero duration also made seekFraction() bail out, which
    // is why the slider did nothing once a clip had played to the end.
    if (!m_mpv || m_state == MediaState::Idle || m_state == MediaState::Failed ||
        m_state == MediaState::Ended)
        return;

    const double duration = getDouble("duration");
    if (differs(duration, m_duration)) {
        m_duration = duration;
        emit durationChanged(m_duration);
    }
    const double position = getDouble("time-pos");
    if (differs(position, m_position)) {
        m_position = position;
        emit positionChanged(m_position);
    }
    if (m_state == MediaState::Playing || m_state == MediaState::Paused)
        setState(getFlag("pause") ? MediaState::Paused : MediaState::Playing);

    const QSize size(static_cast<int>(getInt("width")), static_cast<int>(getInt("height")));
    if (size != m_videoSize) {
        m_videoSize = size;
        emit videoSizeChanged(m_videoSize);
    }
    m_videoCodec = getString("video-codec");
    refreshMetadata();
}

void MpvMediaEngine::refreshMetadata() {
    static const char *const keys[] = {"title", "artist", "album", "date",
                                       "genre", "track",  "comment"};
    QHash<QString, QString> metadata;
    for (const char *key : keys) {
        const QString value = getString((QByteArray("metadata/by-key/") + key).constData());
        if (!value.isEmpty())
            metadata.insert(QString::fromLatin1(key), value);
    }
    if (metadata == m_metadata)
        return;
    m_metadata = metadata;
    emit metadataChanged(m_metadata);
}

void MpvMediaEngine::setState(MediaState state) {
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged(m_state);
}

void MpvMediaEngine::clearObservedValues() {
    m_duration = 0.0;
    m_position = 0.0;
    m_videoSize = {};
    m_videoCodec.clear();
    if (!m_metadata.isEmpty()) {
        m_metadata.clear();
        emit metadataChanged(m_metadata);
    }
    emit durationChanged(0.0);
    emit positionChanged(0.0);
    emit videoSizeChanged({});
}

double MpvMediaEngine::getDouble(const char *property) const {
    double value = 0.0;
    return m_mpv && mpv_get_property(m_mpv, property, MPV_FORMAT_DOUBLE, &value) >= 0
               ? value
               : 0.0;
}

long long MpvMediaEngine::getInt(const char *property) const {
    long long value = 0;
    return m_mpv && mpv_get_property(m_mpv, property, MPV_FORMAT_INT64, &value) >= 0
               ? value
               : 0;
}

bool MpvMediaEngine::getFlag(const char *property) const {
    int value = 0;
    return m_mpv && mpv_get_property(m_mpv, property, MPV_FORMAT_FLAG, &value) >= 0 &&
           value != 0;
}

QString MpvMediaEngine::getString(const char *property) const {
    if (!m_mpv)
        return {};
    char *value = mpv_get_property_string(m_mpv, property);
    if (!value)
        return {};
    const QString result = QString::fromUtf8(value);
    mpv_free(value);
    return result;
}

mpv_handle *MpvMediaEngine::handle() const { return m_mpv; }

void MpvMediaEngine::videoSurfaceReady() {
    m_surfaceReady = true;
    if (!m_pendingVideoLoad)
        return;
    m_pendingVideoLoad = false;
    issueLoad();
}
