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
    bool eofReached() const; // at end-of-file
    bool ended() const;      // playback finished / core is idle (no active file)
    int videoWidth() const;
    int videoHeight() const;
    QString videoCodec() const;

signals:
    // Emitted (queued) from mpv's render thread to request a repaint.
    void updateRequested();
    // Emitted (queued) from mpv's thread when core events are pending.
    void mpvEvents();

private slots:
    void doUpdate();
    void onMpvEvents(); // drains the mpv event queue on the GUI thread

protected:
    void initializeGL() override;
    void paintGL() override;

private:
    double getDouble(const char *prop) const;
    long long getInt(const char *prop) const;
    bool getFlag(const char *prop) const; // reads a boolean/flag mpv property

    static void *getProcAddress(void *ctx, const char *name);
    static void onMpvRenderUpdate(void *ctx);
    static void onMpvWakeup(void *ctx);

    mpv_handle *m_mpv = nullptr;
    mpv_render_context *m_mpvGl = nullptr;
    // A load() requested before the GL/render context exists is deferred here
    // and replayed from initializeGL(), so mpv always has a VO when it opens the
    // file (otherwise the video track isn't decoded and the frame stays black).
    QString m_pendingLoad;
    // The most recently loaded file, so playPause() can restart it after EOF.
    QString m_currentPath;
    // Set from the mpv END_FILE(EOF) event, cleared on START_FILE / load(). The
    // authoritative "playback finished" signal (property polling is unreliable
    // without draining the event queue).
    bool m_ended = false;
};
