#pragma once

#include <QOpenGLWidget>
#include <QString>

struct mpv_handle;
struct mpv_render_context;

// Thin QOpenGLWidget wrapper around libmpv's render API (OpenGL backend).
//
// libmpv renders straight into the widget's framebuffer via
// mpv_render_context_render(), which is the most robust path under compositing
// window managers (e.g. deepin/dxcb). The render context's update callback is
// invoked from mpv's own thread, so it only emits a queued signal that hops
// back to the GUI thread to schedule a repaint.
class MpvWidget : public QOpenGLWidget {
    Q_OBJECT

public:
    explicit MpvWidget(QWidget *parent = nullptr);
    ~MpvWidget() override;

    // Playback control.
    void load(const QString &path);
    void stop();          // unload the current file; safe to call when idle
    void playPause();     // toggle the pause flag
    void setSpeed(double speed);
    void seekFraction(double fraction); // absolute seek to fraction*duration
    void setVolume(int volume);         // 0..100
    void setMute(bool mute);

    // Property queries (return sensible defaults when unavailable).
    double durationSeconds() const;
    double positionSeconds() const;
    bool paused() const;
    int videoWidth() const;
    int videoHeight() const;
    QString videoCodec() const;

signals:
    // Emitted (queued) from mpv's render thread to request a repaint.
    void updateRequested();

private slots:
    void doUpdate();

protected:
    void initializeGL() override;
    void paintGL() override;

private:
    double getDouble(const char *prop) const;
    long long getInt(const char *prop) const;

    static void *getProcAddress(void *ctx, const char *name);
    static void onMpvRenderUpdate(void *ctx);

    mpv_handle *m_mpv = nullptr;
    mpv_render_context *m_mpvGl = nullptr;
};
