#pragma once

#include <QOpenGLWidget>

class MpvMediaEngine;
struct mpv_render_context;

class MpvVideoSurface final : public QOpenGLWidget {
public:
    explicit MpvVideoSurface(MpvMediaEngine &engine, QWidget *parent = nullptr);
    ~MpvVideoSurface() override;

protected:
    void initializeGL() override;
    void paintGL() override;

private:
    static void *getProcAddress(void *context, const char *name);
    static void onRenderUpdate(void *context);
    void releaseRenderContext();

    MpvMediaEngine &m_engine;
    mpv_render_context *m_renderContext = nullptr;
};
