#pragma once

#include <QList>
#include <QPixmap>
#include <QWidget>

class QMenu;
class QAbstractButton;
class QEvent;
class QLabel;
class QResizeEvent;

// Self-drawn window title bar for the frameless MainWindow: the app icon on the
// left, menu buttons (Commands / View) next to it, and minimize /
// maximize-restore / close buttons on the right. Colours follow the active
// theme. Dragging the empty area moves the window (startSystemMove);
// double-clicking toggles maximize. The menus are rendered as flat tool buttons
// rather than a QMenuBar, which collapses to a ">>" overflow as a layout item.
class TitleBar : public QWidget {
    Q_OBJECT
    // Painted background tile, for a theme with a texture rather than a flat
    // colour. paintEvent draws its own rounded-top shape (the frameless window
    // needs it), so a stylesheet `background:` can never reach this widget --
    // this hook is how a sheet hands it one:
    //
    //     TitleBar { qproperty-backgroundTile: url(:/icons/crt-scan-chrome.png); }
    //
    // Null (the default) keeps the palette's window colour, so light.qss and
    // dark.qss need no changes.
    Q_PROPERTY(QPixmap backgroundTile READ backgroundTile WRITE setBackgroundTile)

public:
    TitleBar(QWidget *window, const QList<QMenu *> &menus, QWidget *parent = nullptr);

    QPixmap backgroundTile() const { return m_backgroundTile; }
    void setBackgroundTile(const QPixmap &tile);

    // Refreshes the maximize/restore glyph after an external state change.
    void syncWindowState();

    // Shows or hides the "New Version" badge. Hidden by default; MainWindow calls
    // this with true once the daily update check (or a manual one) finds a newer
    // release, and the badge's click emits updateRequested().
    void setUpdateAvailable(bool available);

signals:
    // Emitted when the "New Version" badge is clicked (opens the update dialog).
    void updateRequested();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

    QPixmap m_backgroundTile;

private:
    void positionTitle();
    void syncWindowIcon();

    QWidget *m_window;
    QLabel *m_icon = nullptr;
    QLabel *m_title = nullptr;
    QAbstractButton *m_maxButton = nullptr; // a TitleButton (cast in the .cpp)
    QAbstractButton *m_updateBadge = nullptr; // "New Version" badge, hidden until an update is found
    bool m_pressed = false;                 // left button down (for drag vs double-click)
};
