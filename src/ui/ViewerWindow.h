#pragma once

#include <QFont>
#include <QString>

#include "FramelessWindow.h"

class Settings;
class QuickView;

// Top-level window that hosts a QuickView (in Window context) as the F3 "view
// file" surface, so the F3 viewer and the Ctrl+Q preview pane share one
// implementation and the same capabilities (image/text/video/PDF/markdown/
// office/archive). Deletes itself on close; Esc closes it.
//
// FramelessWindow, not a bare QWidget: this window used to keep the native
// decorations, which ignore the theme entirely -- a stock light-grey title bar
// sat on top of a fully themed body. It is a base-class swap only; the layout
// below is unchanged.
class ViewerWindow : public FramelessWindow {
    Q_OBJECT

public:
    // `editing` opens straight into the editor rather than the preview.
    ViewerWindow(Settings &settings, const QString &path, QWidget *parent = nullptr,
                 bool editing = false);

    // Forwards a theme / "tint images" change to the preview this window wraps.
    // A viewer opened with F3 is a separate top-level window, so MainWindow has
    // to reach it explicitly -- nothing else knows it exists.
    void refreshPhosphor();

    // Same reason refreshPhosphor() exists: this window is invisible to the
    // usual chrome-font pass, so MainWindow forwards the interface font here.
    void applyChromeFont(const QFont &font);

    // Hides the preview's Edit button. Passed false when the window was opened
    // on a downloaded copy, which is not editable in place.
    void setEditingEnabled(bool enabled);

    // Switches the preview into the editor, in place. Same window either way --
    // F4 opens one of these already in edit mode instead of a second window.
    bool beginEditing(const QString &path);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    QuickView *m_preview;
};
