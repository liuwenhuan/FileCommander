#pragma once

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

private:
    QuickView *m_preview;
};
