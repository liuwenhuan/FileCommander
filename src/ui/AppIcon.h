#pragma once

#include <QColor>
#include <QIcon>

namespace ttc {

// The application icon, painted in code (rather than looked up in the desktop's
// icon theme or loaded from an SVG resource) so a title-bar/taskbar icon is
// guaranteed on every desktop, independent of the icon theme or the Qt SVG
// plugin.
//
// `tint` recolours it through the same luma-to-phosphor map as every other
// surface (fc::tintImage) rather than a second, hand-picked palette. That is
// deliberate: the icon sits in the title bar right next to the themed chrome
// glyphs, and two colours chosen independently would not match -- ThemeManager
// hands it exactly the glyph colour for that reason.
//
// Pass an invalid colour -- the default -- for the stock blue icon. Only
// main.cpp does, for the window icon that exists before any theme is applied.
//
// Lives in `ui`, not in main.cpp, because the theme can change at runtime and
// ThemeManager has to be able to repaint it.
enum class AppIconStyle {
    // The mark as drawn: a filled body with two panes on it. Recoloured by
    // brightness, so it stays readable as long as the tint is LIGHTER than what
    // it sits on -- phosphor on black glass, near-white on dark grey.
    Filled,
    // The same composition in strokes on nothing. For a tint DARKER than its
    // surface: flattening a filled mark to #404040 on a white title bar puts a
    // near-black slab beside line glyphs drawn at two pixels wide, and it reads
    // as the heaviest thing in the window rather than as part of it.
    Outline,
};
QIcon appIcon(const QColor &tint = QColor(), AppIconStyle style = AppIconStyle::Filled);

} // namespace ttc
