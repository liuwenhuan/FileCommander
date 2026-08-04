#pragma once

#include <QColor>
#include <QPixmap>
#include <QWidget>

#include "DialogTitleBar.h"

// A frameless TOP-LEVEL QWidget wearing the same self-drawn chrome as the main
// window and the dialogs: translucent drop shadow, rounded corners, WM-driven
// edge resize, and a slim theme-following title bar.
//
// FramelessDialog is the same idea for a QDialog. The difference that earns a
// second class is the title bar's controls: a dialog only closes, whereas these
// windows (the F3 viewer and the F4 editor) are ordinary resizable windows and
// must keep minimize, maximize/restore, double-click-to-maximize and the WM's
// snap gestures. So this one asks DialogTitleBar for the full set, and collapses
// the shadow margin (and the rounded corners with it) while maximized, exactly
// as MainWindow does -- otherwise a maximized window would show a transparent
// gap at the screen edges.
//
// Conversion is a base-class swap only, as with FramelessDialog: a subclass that
// does `new QVBoxLayout(this)` keeps working, because the base reserves the
// title bar and shadow margins with setContentsMargins() and Qt lays the
// subclass's layout out inside the resulting contentsRect().
//
// Qt::Window is set here, and the minimize/maximize/close hints with it, so the
// window stays an ordinary managed window: taskbar and alt-tab entry, WM window
// menu, and (on Windows) the snap gestures that WS_MINIMIZEBOX/WS_MAXIMIZEBOX
// enable. Frameless removes the decorations, not the window's status.
class FramelessWindow : public QWidget {
    Q_OBJECT
    // Supplies the CRT texture to the self-painted window body and forwards it
    // to the title bar. A null tile restores the palette background.
    Q_PROPERTY(QPixmap backgroundTile READ backgroundTile WRITE setBackgroundTile)
public:
    explicit FramelessWindow(QWidget *parent = nullptr);

    QPixmap backgroundTile() const { return m_backgroundTile; }
    void setBackgroundTile(const QPixmap &tile);

    DialogTitleBar *titleBar() const { return m_titleBar; }

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;
    bool event(QEvent *event) override;

private:
    void updateTitleBarLayout();

    DialogTitleBar *m_titleBar = nullptr;
    QPixmap m_backgroundTile;
    QPixmap m_frameCache;
    QColor m_frameCacheColor;
};
