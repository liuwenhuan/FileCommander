#pragma once

#include <QColor>
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
    // preview pane. Each recolours to a bright member of the theme's own
    // palette -- see the note in apply() for why the colour must be bright, and
    // for why the light theme therefore names none, leaving both switches with
    // nothing to do. Chrome glyphs follow the theme regardless of either.
    void apply(Settings::Theme theme, bool phosphorImages = true,
               bool phosphorPreview = true);
    Settings::Theme requestedTheme() const { return m_requestedTheme; }
    bool phosphorImages() const { return m_phosphorImages; }
    bool phosphorPreview() const { return m_phosphorPreview; }
    // The colour the two switches recolour content to under the theme last
    // applied, invalid where the theme names none. Menus read it to show the
    // switches as unavailable rather than as doing nothing.
    QColor contentTint() const { return m_contentTint; }

private:
    bool systemPrefersDark() const;

    Settings::Theme m_requestedTheme = Settings::Theme::Auto;
    bool m_phosphorImages = true;
    bool m_phosphorPreview = true;
    QColor m_contentTint;
    QPalette m_originalPalette;
};
