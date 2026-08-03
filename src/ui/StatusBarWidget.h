#pragma once

#include <QWidget>

class QLabel;
class QToolButton;

// Bottom status strip: selection summary for the active panel.
class StatusBarWidget : public QWidget {
    Q_OBJECT

public:
    explicit StatusBarWidget(QWidget *parent = nullptr);

    void setSelectionInfo(int selectedCount, qint64 selectedBytes, int totalCount);
    void setDiskInfo(qint64 freeBytes, qint64 totalBytes);

    // Connection status shown centred in the strip, for network tabs. `level`:
    // 0 = hide (local tab / connected), 1 = connecting (grey), 2 = reconnecting
    // (amber), 3 = failed (red, with a clickable "Retry"), 4 = needs auth (amber,
    // with a clickable "登录" login link). Empty text also hides. Levels 3 and 4
    // both emit retryRequested on click; the panel decides retry vs re-login.
    enum ConnLevel {
        ConnNone = 0,
        ConnConnecting = 1,
        ConnReconnecting = 2,
        ConnFailed = 3,
        ConnNeedsAuth = 4,
        // Neutral, non-actionable note about a connection that IS working, e.g.
        // that it adopted a session someone else's credentials opened. Renders
        // muted with no link -- there is nothing to retry.
        ConnNotice = 5
    };
    void setConnectionStatus(const QString &text, int level);

signals:
    // The trailing "-"/"+" buttons: shrink/grow the panel's current view (list
    // row height or thumbnail size, depending on mode). FilePanel owns the
    // mode-specific logic; this widget only reports the click.
    void zoomOutRequested();
    void zoomInRequested();
    // The "Retry" link in the failed connection status was clicked.
    void retryRequested();

private:
    QLabel *m_label;
    QLabel *m_connLabel; // centred connection status (network tabs only)
    QLabel *m_diskLabel;
    QToolButton *m_zoomOutButton;
    QToolButton *m_zoomInButton;
};
