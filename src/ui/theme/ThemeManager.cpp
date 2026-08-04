#include "ThemeManager.h"

#include <QApplication>
#include <QFile>
#include <QPixmap>
#include <QScreen>
#include <QWidget>

#include "AppIcon.h"
#include "DialogTitleBar.h"
#include "FramelessDialog.h"
#include "TabBar.h"
#include "TitleBar.h"
#include "filesystem/IconCache.h"
#include "theme/Phosphor.h"

namespace {
// The CRT theme's lit phosphor -- the same #33ff88 green.qss paints text in.
// Kept here (not parsed out of the stylesheet) because the recolouring happens
// in painting code that never reads the stylesheet; if one is changed the other
// must be too.
const QColor kPhosphor(0x33, 0xff, 0x88);
} // namespace

ThemeManager::ThemeManager(QObject *parent) : QObject(parent) {
    // Must capture this before any stylesheet is ever applied, since a
    // stylesheet can itself alter qApp->palette().
    m_originalPalette = qApp->palette();
}

bool ThemeManager::systemPrefersDark() const {
    return m_originalPalette.color(QPalette::Window).lightness() < 128;
}

void ThemeManager::apply(Settings::Theme theme, bool phosphorImages, bool phosphorPreview) {
    m_requestedTheme = theme;
    m_phosphorImages = phosphorImages;
    m_phosphorPreview = phosphorPreview;

    Settings::Theme effective = theme;
    if (effective == Settings::Theme::Auto)
        effective = systemPrefersDark() ? Settings::Theme::Dark : Settings::Theme::Light;

    QString path = ":/themes/light.qss";
    if (effective == Settings::Theme::Dark)
        path = ":/themes/dark.qss";
    else if (effective == Settings::Theme::Crt)
        path = ":/themes/green.qss";

    if (effective == Settings::Theme::Crt) {
        TabBar::setThemeColors(kPhosphor, QColor(0x1f, 0xa8, 0x5c), kPhosphor,
                               kPhosphor, QColor(0x7c, 0xe8, 0xac),
                               QColor(0x12, 0x60, 0x2f));
    } else if (effective == Settings::Theme::Dark) {
        TabBar::setThemeColors(QColor(0x3d, 0x7d, 0xeb), QColor(0xe0, 0xe0, 0xe0),
                               QColor(0xe0, 0xe0, 0xe0), QColor(0xe0, 0x4b, 0x4b),
                               QColor(0xe0, 0x4b, 0x4b), QColor(0x77, 0x77, 0x77));
    } else {
        TabBar::setThemeColors(QColor(0x3d, 0x7d, 0xeb), QColor(0x20, 0x20, 0x20),
                               QColor(0x20, 0x20, 0x20), QColor(0xe0, 0x4b, 0x4b),
                               QColor(0xe0, 0x4b, 0x4b), QColor(0xa0, 0xa0, 0xa0));
    }
    QFile file(path);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text))
        qApp->setStyleSheet(QString::fromUtf8(file.readAll()));

    const bool crt = effective == Settings::Theme::Crt;

    // Content follows its own switches rather than the theme, since recolouring
    // the user's own photographs is a taste decision in a way that recolouring a
    // glyph is not -- and it is two switches, because the file list's pictures
    // and the picture someone opened to LOOK at are different decisions.
    //
    // Neither surface is quantised any more. The coarse raster was atmosphere,
    // and it cost more than it bought: a file-type badge shrank to a handful of
    // cells and stopped being identifiable at list-view sizes, and a thumbnail
    // is there to be recognised before it is there to look period-correct. The
    // hue alone carries the theme. fc::pixelate() and the block parameters stay
    // -- they are what the preview scanline treatment is built alongside -- but
    // nothing enables them for icons or content.

    // Our own SVG chrome is drawn in a mid grey (#888888) that reads as "dark"
    // on a dark background -- the drive and computer glyphs were noticeably
    // dimmer than the text beside them. Every theme names a colour for those,
    // so they sit at the same weight as the text they label. They carry no
    // colour of their own, so there is nothing to lose by recolouring them.
    //
    // The system's FILE icons are a different matter and follow further below:
    // they are full-colour artwork, and putting this same tint on them turned
    // every folder in the light theme into a dark grey slab.
    QColor glyphTint;
    switch (effective) {
    case Settings::Theme::Crt:
        glyphTint = kPhosphor;
        break;
    case Settings::Theme::Dark:
        glyphTint = QColor(0xe0, 0xe0, 0xe0); // dark.qss's text colour
        break;
    case Settings::Theme::Light:
    case Settings::Theme::Auto:
        glyphTint = QColor(0x40, 0x40, 0x40); // darker than #888 on white
        break;
    }
    IconCache::instance().setGlyphTint(glyphTint);

    // File icons and thumbnails together, under the images switch: they sit in
    // the same grid, and "the pictures in the file list match the theme" is one
    // idea whether a cell shows a generated thumbnail or a type icon. CRT only
    // either way -- collapsing every icon to one hue IS that theme, since a
    // stylesheet cannot reach the system icon theme and a surviving blue folder
    // would be the one thing giving it away.
    const QColor imageTint = crt && phosphorImages ? kPhosphor : QColor();
    IconCache::instance().setFileIconTint(imageTint, 0);
    fc::setThumbnailTint(imageTint);
    fc::setPreviewTint(crt && phosphorPreview ? kPhosphor : QColor());
    fc::setContentPixelBlock(0);

    // The app icon is painted by us, so it is repainted rather than recoloured
    // in place. Every top-level window carries its own copy (the frameless
    // TitleBar reads window->windowIcon()), so they all have to be re-set.
    const QIcon icon = ttc::appIcon(crt ? kPhosphor : QColor());
    qApp->setWindowIcon(icon);
    for (QWidget *w : qApp->topLevelWidgets()) {
        w->setWindowIcon(icon);
        if (!crt) {
            if (auto *dialog = qobject_cast<FramelessDialog *>(w))
                dialog->setBackgroundTile(QPixmap());
            for (FramelessDialog *dialog : w->findChildren<FramelessDialog *>())
                dialog->setBackgroundTile(QPixmap());
            for (TitleBar *titleBar : w->findChildren<TitleBar *>())
                titleBar->setBackgroundTile(QPixmap());
            for (DialogTitleBar *titleBar : w->findChildren<DialogTitleBar *>())
                titleBar->setBackgroundTile(QPixmap());
        }
    }
}
