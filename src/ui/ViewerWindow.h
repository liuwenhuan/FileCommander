#pragma once

#include <QFont>
#include <QString>
#include <QWidget>

class Settings;
class QuickView;

// Top-level window that hosts a QuickView (in Window context) as the F3 "view
// file" surface, so the F3 viewer and the Ctrl+Q preview pane share one
// implementation and the same capabilities (image/text/video/PDF/markdown/
// office/archive). Deletes itself on close; Esc closes it.
class ViewerWindow : public QWidget {
    Q_OBJECT

public:
    ViewerWindow(Settings &settings, const QString &path, QWidget *parent = nullptr);

    // Forwards a theme / "tint images" change to the preview this window wraps.
    // A viewer opened with F3 is a separate top-level window, so MainWindow has
    // to reach it explicitly -- nothing else knows it exists.
    void refreshPhosphor();

    // Same reason refreshPhosphor() exists: this window is invisible to the
    // usual chrome-font pass, so MainWindow forwards the interface font here.
    void applyChromeFont(const QFont &font);

private:
    QuickView *m_preview;
};
