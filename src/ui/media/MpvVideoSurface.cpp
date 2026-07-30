#include "MpvVideoSurface.h"

#include "MpvMediaEngine.h"

#include <QDebug>
#include <QMetaObject>
#include <QOpenGLContext>

#include <mpv/render_gl.h>

MpvVideoSurface::MpvVideoSurface(MpvMediaEngine &engine, QWidget *parent)
    : QOpenGLWidget(parent), m_engine(engine) {}

MpvVideoSurface::~MpvVideoSurface() {
    makeCurrent();
    if (QOpenGLContext *glContext = context())
        disconnect(glContext, nullptr, this, nullptr);
    releaseRenderContext();
    doneCurrent();
}

void *MpvVideoSurface::getProcAddress(void *, const char *name) {
    QOpenGLContext *glContext = QOpenGLContext::currentContext();
    return glContext
               ? reinterpret_cast<void *>(glContext->getProcAddress(QByteArray(name)))
               : nullptr;
}

void MpvVideoSurface::onRenderUpdate(void *context) {
    auto *surface = static_cast<MpvVideoSurface *>(context);
    QMetaObject::invokeMethod(surface, [surface]() { surface->update(); },
                              Qt::QueuedConnection);
}

void MpvVideoSurface::releaseRenderContext() {
    if (!m_renderContext)
        return;
    mpv_render_context_set_update_callback(m_renderContext, nullptr, nullptr);
    mpv_render_context_free(m_renderContext);
    m_renderContext = nullptr;
}

void MpvVideoSurface::initializeGL() {
    releaseRenderContext();
    mpv_handle *mpv = m_engine.handle();
    if (!mpv)
        return;

    mpv_opengl_init_params glInit{};
    glInit.get_proc_address = &MpvVideoSurface::getProcAddress;
    glInit.get_proc_address_ctx = this;
    mpv_render_param parameters[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glInit},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    if (mpv_render_context_create(&m_renderContext, mpv, parameters) < 0) {
        qWarning("MpvVideoSurface: failed to create mpv render context");
        return;
    }
    mpv_render_context_set_update_callback(m_renderContext,
                                           &MpvVideoSurface::onRenderUpdate, this);

    if (QOpenGLContext *glContext = context()) {
        connect(glContext, &QOpenGLContext::aboutToBeDestroyed, this,
                [this]() {
                    makeCurrent();
                    releaseRenderContext();
                    doneCurrent();
                },
                Qt::DirectConnection);
    }
    m_engine.videoSurfaceReady();
}

void MpvVideoSurface::paintGL() {
    if (!m_renderContext)
        return;

    mpv_opengl_fbo framebuffer{};
    framebuffer.fbo = static_cast<int>(defaultFramebufferObject());
    framebuffer.w = qRound(width() * devicePixelRatioF());
    framebuffer.h = qRound(height() * devicePixelRatioF());
    int flipY = 1;
    mpv_render_param parameters[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &framebuffer},
        {MPV_RENDER_PARAM_FLIP_Y, &flipY},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    mpv_render_context_render(m_renderContext, parameters);
}
