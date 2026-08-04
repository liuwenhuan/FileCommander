#pragma once

#include <QObject>
#include <QPalette>

#include "Settings.h"

// Applies the light/dark/auto QSS theme application-wide. "Auto" follows
// the desktop's own light/dark preference, inferred from the default
// QPalette captured before any stylesheet is applied (Qt5 has no
// QStyleHints::colorScheme() -- that's Qt6 -- so this is a lightness
// heuristic on the system window color instead).
class ThemeManager : public QObject {
    Q_OBJECT

public:
    explicit ThemeManager(QObject *parent = nullptr);

    // The two content switches, from Settings: `phosphorImages` covers the file
    // list's pictures (thumbnails and file-type icons), `phosphorPreview` the
    // preview pane. Both apply under every theme, each recolouring to a bright
    // member of that theme's own palette -- see the note in apply() for why the
    // colour must be bright. Chrome glyphs follow the theme regardless of
    // either.
    void apply(Settings::Theme theme, bool phosphorImages = true,
               bool phosphorPreview = true);
    Settings::Theme requestedTheme() const { return m_requestedTheme; }
    bool phosphorImages() const { return m_phosphorImages; }
    bool phosphorPreview() const { return m_phosphorPreview; }

private:
    bool systemPrefersDark() const;

    Settings::Theme m_requestedTheme = Settings::Theme::Auto;
    bool m_phosphorImages = true;
    bool m_phosphorPreview = true;
    QPalette m_originalPalette;
};
