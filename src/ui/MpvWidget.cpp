#include "MpvWidget.h"

#include <QOpenGLContext>
#include <cstring>
#include <stdexcept>

#include <mpv/client.h>
#include <mpv/render_gl.h>

MpvWidget::MpvWidget(QWidget *parent) : QOpenGLWidget(parent) {
    m_mpv = mpv_create();
    if (!m_mpv)
        throw std::runtime_error("could not create mpv context");

    // Keep the core lean and quiet: no on-screen controls, no config files,
    // start paused-friendly and muted (the UI exposes explicit controls).
    mpv_set_option_string(m_mpv, "config", "no");
    mpv_set_option_string(m_mpv, "osc", "no");
    mpv_set_option_string(m_mpv, "input-default-bindings", "no");
    mpv_set_option_string(m_mpv, "input-vo-keyboard", "no");
    mpv_set_option_string(m_mpv, "keep-open", "yes"); // don't tear down at EOF
    mpv_set_option_string(m_mpv, "mute", "yes");      // default muted

    if (mpv_initialize(m_mpv) < 0)
        throw std::runtime_error("could not initialize mpv");

    // Queued so the GUI thread owns the actual update()/repaint.
    connect(this, &MpvWidget::updateRequested, this, &MpvWidget::doUpdate,
            Qt::QueuedConnection);
}

MpvWidget::~MpvWidget() {
    // The render context must be torn down with a current GL context before the
    // core handle is destroyed.
    makeCurrent();
    if (m_mpvGl)
        mpv_render_context_free(m_mpvGl);
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

void MpvWidget::initializeGL() {
    mpv_opengl_init_params glInit{};
    glInit.get_proc_address = &MpvWidget::getProcAddress;
    glInit.get_proc_address_ctx = this;

    int advancedControl = 1;
    mpv_render_param params[]{
        {MPV_RENDER_PARAM_API_TYPE,
         const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glInit},
        {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advancedControl},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };

    if (mpv_render_context_create(&m_mpvGl, m_mpv, params) < 0)
        throw std::runtime_error("failed to initialize mpv GL context");

    mpv_render_context_set_update_callback(m_mpvGl,
                                           &MpvWidget::onMpvRenderUpdate, this);
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
    const QByteArray file = path.toUtf8();
    const char *cmd[] = {"loadfile", file.constData(), nullptr};
    mpv_command_async(m_mpv, 0, cmd);
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
    int paused = getInt("pause") != 0;
    int flag = paused ? 0 : 1;
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

double MpvWidget::durationSeconds() const {
    return getDouble("duration");
}

double MpvWidget::positionSeconds() const {
    return getDouble("time-pos");
}

bool MpvWidget::paused() const {
    return getInt("pause") != 0;
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
