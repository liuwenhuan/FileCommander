#include "MpvWidget.h"

#include "MpvStreamSource.h"

#include <QDebug>
#include <QOpenGLContext>
#include <cstring>
#include <stdexcept>

#include <mpv/client.h>
#include <mpv/render_gl.h>

MpvWidget::MpvWidget(QWidget *parent) : QOpenGLWidget(parent) {
    m_mpv = mpv_create();
    if (!m_mpv)
        throw std::runtime_error("could not create mpv context");

    // Keep the core lean and quiet: no on-screen controls, no config files.
    // QuickView applies the user-persisted mute/volume state before loading.
    mpv_set_option_string(m_mpv, "config", "no");
    mpv_set_option_string(m_mpv, "osc", "no");
    mpv_set_option_string(m_mpv, "input-default-bindings", "no");
    mpv_set_option_string(m_mpv, "input-vo-keyboard", "no");
    // NOTE: no keep-open. Holding the file open at EOF (keep-open=yes) left the
    // libmpv VO in a state where the *next* loadfile decoded audio but rendered
    // a black frame. Letting the clip end cleanly means each new load fully
    // re-inits the video chain; replay is handled by reloading (see playPause).
    mpv_set_option_string(m_mpv, "mute", "no"); // default unmuted; QuickView overrides from Settings
    // Render into our QOpenGLWidget via the render API instead of letting mpv
    // spawn its own top-level window. Without this, mpv picks a windowed VO
    // (gpu) and the video pops out in a separate "… - mpv" window.
    mpv_set_option_string(m_mpv, "vo", "libmpv");

    // Read-ahead, bounded. A file streamed off a network backend has to buffer
    // or every decode stalls on a round trip -- but mpv's own default is to
    // read ahead up to 150 MB, which for most previewed files is the entire
    // download this whole path exists to avoid. Measured on a 133 MB clip:
    // cache=yes alone pulled 130 MB within five seconds of starting, while the
    // same five seconds bounded to 8 MiB pulled 8.7 MB. Local files are
    // unaffected in practice (seeking a local file is free, and the old 150 MB
    // ceiling only ever cost memory).
    mpv_set_option_string(m_mpv, "cache", "yes");
    mpv_set_option_string(m_mpv, "demuxer-max-bytes", "8MiB");

    if (mpv_initialize(m_mpv) < 0)
        throw std::runtime_error("could not initialize mpv");

    // Lets load() accept a MpvStreamSource URL, which reads a remote file
    // through its FileProvider instead of downloading it first.
    if (!MpvStreamSource::registerProtocol(m_mpv))
        qWarning("MpvWidget: libmpv refused the stream protocol; remote video will download");

    // Queued so the GUI thread owns the actual update()/repaint.
    connect(this, &MpvWidget::updateRequested, this, &MpvWidget::doUpdate,
            Qt::QueuedConnection);
    // Drain core events on the GUI thread; the wakeup callback fires on mpv's
    // own thread and only bounces a queued signal.
    connect(this, &MpvWidget::mpvEvents, this, &MpvWidget::onMpvEvents,
            Qt::QueuedConnection);
    mpv_set_wakeup_callback(m_mpv, &MpvWidget::onMpvWakeup, this);
}

MpvWidget::~MpvWidget() {
    // Stop the wakeup callback first so it can't bounce a signal at a dying object.
    if (m_mpv)
        mpv_set_wakeup_callback(m_mpv, nullptr, nullptr);
    // The render context must be torn down with a current GL context before the
    // core handle is destroyed.
    makeCurrent();
    // Drop the aboutToBeDestroyed cleanup so it can't fire against this
    // half-destroyed object as the base QOpenGLWidget tears its context down.
    if (QOpenGLContext *ctx = context())
        disconnect(ctx, nullptr, this, nullptr);
    if (m_mpvGl) {
        mpv_render_context_free(m_mpvGl);
        m_mpvGl = nullptr;
    }
    if (m_mpv)
        mpv_terminate_destroy(m_mpv);
}

void *MpvWidget::getProcAddress(void *ctx, const char *name) {
    Q_UNUSED(ctx);
    QOpenGLContext *glctx = QOpenGLContext::currentContext();
    if (!glctx)
        return nullptr;
    return reinterpret_cast<void *>(glctx->getProcAddress(QByteArray(name)));
}

void MpvWidget::onMpvRenderUpdate(void *ctx) {
    // Called from mpv's render thread: only bounce a queued signal.
    auto *self = static_cast<MpvWidget *>(ctx);
    emit self->updateRequested();
}

void MpvWidget::onMpvWakeup(void *ctx) {
    // Called from mpv's thread: only bounce a queued signal to the GUI thread.
    auto *self = static_cast<MpvWidget *>(ctx);
    emit self->mpvEvents();
}

void MpvWidget::onMpvEvents() {
    if (!m_mpv)
        return;
    // Drain everything currently queued (0 timeout = non-blocking).
    while (true) {
        mpv_event *ev = mpv_wait_event(m_mpv, 0);
        if (!ev || ev->event_id == MPV_EVENT_NONE)
            break;
        switch (ev->event_id) {
        case MPV_EVENT_START_FILE:
            m_ended = false;
            break;
        case MPV_EVENT_END_FILE: {
            // Only a natural end counts as "finished"; a stop/loadfile-driven
            // end (reason != EOF) is just a transition to the next clip.
            auto *ef = static_cast<mpv_event_end_file *>(ev->data);
            if (ef && ef->reason == MPV_END_FILE_REASON_EOF)
                m_ended = true;
            // ERROR is the one reason that means the file never played: a
            // stream whose backend refused, an unreadable path, an unknown
            // format. STOP and REDIRECT are our own doing and must not be
            // mistaken for it.
            else if (ef && ef->reason == MPV_END_FILE_REASON_ERROR)
                emit loadFailed(m_currentPath);
            break;
        }
        default:
            break;
        }
    }
}

void MpvWidget::initializeGL() {
    // initializeGL runs again every time the backing GL context is rebuilt —
    // which happens whenever the widget is reparented (e.g. Ctrl+Q swapping the
    // preview in and out of the splitter). mpv permits only ONE render context
    // per handle, so a stale one left over from the previous GL context must be
    // freed first; creating a second otherwise fails.
    if (m_mpvGl) {
        mpv_render_context_free(m_mpvGl);
        m_mpvGl = nullptr;
    }

    mpv_opengl_init_params glInit{};
    glInit.get_proc_address = &MpvWidget::getProcAddress;
    glInit.get_proc_address_ctx = this;

    // Plain (non-advanced) render mode: mpv decodes on its own clock and the
    // update callback just asks us to repaint. Advanced control requires the
    // render calls themselves to drive decoding, which left the frame black and
    // video params unread when the timing wasn't perfectly wired.
    mpv_render_param params[]{
        {MPV_RENDER_PARAM_API_TYPE,
         const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glInit},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };

    // Never throw from a GL callback: an exception unwinding through Qt's paint
    // machinery calls std::terminate and takes the whole app down (this was the
    // repeated-Ctrl+Q crash). Log and bail instead.
    if (mpv_render_context_create(&m_mpvGl, m_mpv, params) < 0) {
        m_mpvGl = nullptr;
        qWarning("MpvWidget: failed to create mpv render context");
        return;
    }

    mpv_render_context_set_update_callback(m_mpvGl,
                                           &MpvWidget::onMpvRenderUpdate, this);

    // Free this render context the moment its GL context goes away (reparent or
    // close), so the next initializeGL() starts clean and the destructor can't
    // double-free. Bound to `this` so it's auto-removed when the widget dies.
    if (QOpenGLContext *ctx = context()) {
        connect(
            ctx, &QOpenGLContext::aboutToBeDestroyed, this,
            [this]() {
                makeCurrent();
                if (m_mpvGl) {
                    mpv_render_context_free(m_mpvGl);
                    m_mpvGl = nullptr;
                }
                doneCurrent();
            },
            Qt::DirectConnection);
    }

    // Now that the VO (render context) exists, play any file requested earlier.
    if (!m_pendingLoad.isEmpty()) {
        const QString path = m_pendingLoad;
        m_pendingLoad.clear();
        load(path);
    }
}

void MpvWidget::paintGL() {
    if (!m_mpvGl)
        return;

    mpv_opengl_fbo fbo{};
    fbo.fbo = static_cast<int>(defaultFramebufferObject());
    fbo.w = width() * devicePixelRatioF();
    fbo.h = height() * devicePixelRatioF();

    int flipY = 1; // Qt's framebuffer origin is top-left
    mpv_render_param params[]{
        {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
        {MPV_RENDER_PARAM_FLIP_Y, &flipY},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    mpv_render_context_render(m_mpvGl, params);
}

void MpvWidget::doUpdate() {
    update();
}

void MpvWidget::load(const QString &path) {
    if (!m_mpv)
        return;
    m_currentPath = path; // remembered so playPause() can restart after EOF
    m_ended = false;      // a new file is starting
    if (!m_mpvGl) {
        // GL/render context not created yet; replay once initializeGL() runs.
        m_pendingLoad = path;
        return;
    }
    const QByteArray file = path.toUtf8();
    const char *cmd[] = {"loadfile", file.constData(), nullptr};
    mpv_command_async(m_mpv, 0, cmd);
    // `pause` is a persistent core property: if a previous clip was paused or
    // ended at EOF (keep-open pauses there), it would carry over and freeze this
    // new file. Force playback on every load so a fresh clip always starts.
    int unpaused = 0;
    mpv_set_property_async(m_mpv, 0, "pause", MPV_FORMAT_FLAG, &unpaused);
}

void MpvWidget::applyVideoFilter(const QString &filter) {
    if (m_videoFilter == filter)
        return;
    m_videoFilter = filter;
    if (!m_mpv)
        return;
    // "vf" is a persistent core property, so it survives loadfile and only has
    // to be set when it changes. Setting it while a clip is playing rebuilds
    // the filter chain, which can drop a frame -- acceptable for a theme
    // switch, and the alternative (deferring to the next load) would leave the
    // picture visibly out of step with the rest of the window.
    mpv_set_property_string(m_mpv, "vf", m_videoFilter.toUtf8().constData());
}

void MpvWidget::stop() {
    if (!m_mpv)
        return;
    const char *cmd[] = {"stop", nullptr};
    mpv_command_async(m_mpv, 0, cmd);
}

void MpvWidget::playPause() {
    if (!m_mpv)
        return;
    // Finished (core idle after EOF, since we don't keep-open): reload the file
    // to replay it from the start — there's nothing to unpause otherwise.
    if (ended()) {
        if (!m_currentPath.isEmpty())
            load(m_currentPath);
        return;
    }
    const bool isPaused = getFlag("pause");
    int flag = isPaused ? 0 : 1;
    mpv_set_property_async(m_mpv, 0, "pause", MPV_FORMAT_FLAG, &flag);
}

void MpvWidget::setSpeed(double speed) {
    if (m_mpv)
        mpv_set_property_async(m_mpv, 0, "speed", MPV_FORMAT_DOUBLE, &speed);
}

void MpvWidget::seekFraction(double fraction) {
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

void MpvWidget::setVolume(int volume) {
    if (!m_mpv)
        return;
    double v = volume;
    mpv_set_property_async(m_mpv, 0, "volume", MPV_FORMAT_DOUBLE, &v);
}

void MpvWidget::setMute(bool mute) {
    if (!m_mpv)
        return;
    int flag = mute ? 1 : 0;
    mpv_set_property_async(m_mpv, 0, "mute", MPV_FORMAT_FLAG, &flag);
}

double MpvWidget::getDouble(const char *prop) const {
    if (!m_mpv)
        return 0.0;
    double value = 0.0;
    if (mpv_get_property(m_mpv, prop, MPV_FORMAT_DOUBLE, &value) < 0)
        return 0.0;
    return value;
}

long long MpvWidget::getInt(const char *prop) const {
    if (!m_mpv)
        return 0;
    long long value = 0;
    if (mpv_get_property(m_mpv, prop, MPV_FORMAT_INT64, &value) < 0)
        return 0;
    return value;
}

bool MpvWidget::getFlag(const char *prop) const {
    // Boolean mpv properties ("pause", "eof-reached", ...) are FLAG-typed;
    // reading them as INT64 fails and silently returns 0, which is exactly the
    // bug that made "pause" look permanently unpaused (so playback could never
    // be resumed). Always read flags with MPV_FORMAT_FLAG.
    if (!m_mpv)
        return false;
    int flag = 0;
    if (mpv_get_property(m_mpv, prop, MPV_FORMAT_FLAG, &flag) < 0)
        return false;
    return flag != 0;
}

double MpvWidget::durationSeconds() const {
    return getDouble("duration");
}

double MpvWidget::positionSeconds() const {
    return getDouble("time-pos");
}

bool MpvWidget::paused() const {
    return getFlag("pause");
}

bool MpvWidget::eofReached() const {
    return getFlag("eof-reached");
}

bool MpvWidget::ended() const {
    // Authoritative: set from the END_FILE(EOF) event drained in onMpvEvents().
    return m_ended;
}

int MpvWidget::videoWidth() const {
    return static_cast<int>(getInt("width"));
}

int MpvWidget::videoHeight() const {
    return static_cast<int>(getInt("height"));
}

QString MpvWidget::videoCodec() const {
    if (!m_mpv)
        return QString();
    char *codec = mpv_get_property_string(m_mpv, "video-codec");
    if (!codec)
        return QString();
    QString result = QString::fromUtf8(codec);
    mpv_free(codec);
    return result;
}
