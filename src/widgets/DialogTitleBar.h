#pragma once

#include <QPixmap>
#include <QWidget>

class QMouseEvent;
class QPaintEvent;
class QAbstractButton;
class QLabel;

// A slim self-drawn title bar for the frameless windows and dialogs. Mirrors the
// main window's TitleBar chrome (theme-following background, rounded top
// corners, centred dimmed title, self-painted window buttons) but without the
// menu bar. The title text and the icon are read live from the host window.
//
// Two control sets, because two kinds of host use it:
//   * CloseOnly     -- FramelessDialog. A dialog only closes, and a
//                      double-click on its bar does nothing.
//   * WindowControls -- FramelessWindow (the F3 viewer, the F4 editor). An
//                      ordinary resizable window, so it keeps minimize,
//                      maximize/restore and double-click-to-maximize.
class DialogTitleBar : public QWidget {
    Q_OBJECT
    // Mirrors TitleBar's CRT texture hook. A null tile keeps the palette window
    // colour for light and dark themes.
    Q_PROPERTY(QPixmap backgroundTile READ backgroundTile WRITE setBackgroundTile)
public:
    enum Controls { CloseOnly, WindowControls };

    explicit DialogTitleBar(QWidget *window, QWidget *parent = nullptr,
                            Controls controls = CloseOnly);

    QPixmap backgroundTile() const { return m_backgroundTile; }
    void setBackgroundTile(const QPixmap &tile);

    Controls controls() const { return m_controls; }

    // Refreshes the maximize/restore glyph after a window-state change. A no-op
    // for a CloseOnly bar.
    void syncWindowState();

signals:
    void heightChanged(int height);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void changeEvent(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    void updateMetrics();
    void syncWindowIcon();
    void toggleMaximized();

    QWidget *m_window = nullptr;
    QLabel *m_icon = nullptr;
    QAbstractButton *m_minButton = nullptr; // a TitleButton (cast in the .cpp)
    QAbstractButton *m_maxButton = nullptr; // ditto; null for CloseOnly
    QAbstractButton *m_closeButton = nullptr;
    QPixmap m_backgroundTile;
    Controls m_controls = CloseOnly;
    bool m_pressed = false;
};
