#pragma once

#include <QDialog>
#include <QPixmap>

class DialogTitleBar;

// A frameless QDialog drop-in that wears the same self-drawn chrome as the main
// window: a translucent drop shadow, rounded corners, and a slim theme-following
// title bar (DialogTitleBar) with a self-painted close button. Native window
// decorations (which ignore the DTK theme and render a too-short light title
// bar) are replaced entirely.
//
// Conversion is a base-class swap only. A subclass that does `new QVBoxLayout(this)`
// keeps working unchanged: the base reserves the title bar and shadow margins via
// setContentsMargins(), and Qt lays out the subclass's layout within the
// resulting contentsRect() — so content automatically flows below the title bar.
// The title bar itself is a manually positioned child (in resizeEvent), sitting
// in the top margin strip outside the content rect, so it never overlaps.
class FramelessDialog : public QDialog {
    Q_OBJECT
    // Supplies the CRT texture to the self-painted dialog body and forwards it
    // to DialogTitleBar. A null tile restores the palette background.
    Q_PROPERTY(QPixmap backgroundTile READ backgroundTile WRITE setBackgroundTile)
public:
    explicit FramelessDialog(QWidget *parent = nullptr);

    QPixmap backgroundTile() const { return m_backgroundTile; }
    void setBackgroundTile(const QPixmap &tile);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;
    bool event(QEvent *event) override;

private:
    void ensureFrameCache();

    DialogTitleBar *m_titleBar = nullptr;
    QPixmap m_backgroundTile;
    QPixmap m_frameCache;
    QColor m_frameCacheColor;
};
