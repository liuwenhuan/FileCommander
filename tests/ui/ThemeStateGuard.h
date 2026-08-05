#pragma once

#include <QApplication>
#include <QColor>
#include <QFont>
#include <QString>

#include "IconCache.h"
#include "theme/Phosphor.h"

// Puts back everything ThemeManager::apply() changes process-wide.
//
// apply() is not a widget operation: it installs an APPLICATION stylesheet, an
// application font, and the tints used to recolour icons, thumbnails and
// preview images. The QApplication outlives every test in the binary, so a test
// that applies a theme and does not undo it is editing the environment of every
// test that runs after it.
//
// Two real failures came from exactly that, both of them "passes alone, fails
// in the full suite", which is the most expensive kind to chase:
//
//   * A leaked stylesheet made FilePanelFontTest see one font-change event on a
//     view's viewport instead of two -- QStyleSheetStyle resolves a font onto a
//     widget at polish time, so the view stops propagating one the way it does
//     with no sheet.
//   * A leaked preview tint repainted a later test's images: a pure green PNG
//     came back as (105, 129, 161), which is green run through a theme's
//     content tint, and QuickViewMotion's fade assertion failed on the colour.
//
// Declare one of these in any test that applies a theme.
class ThemeStateGuard {
public:
    ThemeStateGuard()
        : m_sheet(qApp->styleSheet())
        , m_font(qApp->font())
        , m_thumbnailTint(fc::thumbnailTint())
        , m_previewTint(fc::previewTint())
        , m_glyphTint(IconCache::instance().glyphTint())
        , m_fileIconTint(IconCache::instance().fileIconTint()) {}

    ~ThemeStateGuard() {
        qApp->setStyleSheet(m_sheet);
        qApp->setFont(m_font);
        fc::setThumbnailTint(m_thumbnailTint);
        fc::setPreviewTint(m_previewTint);
        IconCache::instance().setGlyphTint(m_glyphTint);
        IconCache::instance().setFileIconTint(m_fileIconTint);
    }

    ThemeStateGuard(const ThemeStateGuard &) = delete;
    ThemeStateGuard &operator=(const ThemeStateGuard &) = delete;

private:
    QString m_sheet;
    QFont m_font;
    QColor m_thumbnailTint;
    QColor m_previewTint;
    QColor m_glyphTint;
    QColor m_fileIconTint;
};
