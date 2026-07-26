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
// surface (fc::tintImage) instead of a second, hand-picked green palette. That
// is deliberate: the icon sits in the title bar right next to tinted folder
// icons, and two greens chosen independently would not match. Pass an invalid
// colour -- the default -- for the normal blue icon.
//
// Lives in `ui`, not in main.cpp, because the theme can change at runtime and
// ThemeManager has to be able to repaint it.
QIcon appIcon(const QColor &tint = QColor());

} // namespace ttc
