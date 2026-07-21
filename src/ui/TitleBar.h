#pragma once

#include <QList>
#include <QWidget>

class QMenu;
class QAbstractButton;

// Self-drawn window title bar for the frameless MainWindow: the app icon on the
// left, menu buttons (Commands / View) next to it, and minimize /
// maximize-restore / close buttons on the right. Colours follow the active
// theme. Dragging the empty area moves the window (startSystemMove);
// double-clicking toggles maximize. The menus are rendered as flat tool buttons
// rather than a QMenuBar, which collapses to a ">>" overflow as a layout item.
class TitleBar : public QWidget {
    Q_OBJECT

public:
    TitleBar(QWidget *window, const QList<QMenu *> &menus, QWidget *parent = nullptr);

    // Refreshes the maximize/restore glyph after an external state change.
    void syncWindowState();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    QWidget *m_window;
    QAbstractButton *m_maxButton = nullptr; // a TitleButton (cast in the .cpp)
    bool m_pressed = false;                 // left button down (for drag vs double-click)
};
