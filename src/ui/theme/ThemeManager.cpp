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

void ThemeManager::apply(Settings::Theme theme, bool phosphorImages) {
    m_requestedTheme = theme;
    m_phosphorImages = phosphorImages;

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

    // Two separate tints, because they answer to different switches.
    //
    // Chrome (file-type icons, the app icon) follows the THEME: a stylesheet
    // cannot reach the system icon theme, so blue folders and red document
    // icons would otherwise survive into a single-hue theme and be the one
    // thing that gives it away.
    //
    // Content (thumbnails, image/PDF/slide previews, video) follows the
    // TOGGLE, since recolouring the user's own photographs is a taste
    // decision in a way that recolouring a folder glyph is not.
    const qreal dpr = qApp->primaryScreen() ? qApp->primaryScreen()->devicePixelRatio() : 1.0;
    const auto blockFor = [dpr](int logical) { return qMax(2, qRound(logical * dpr)); };

    IconCache::instance().setTint(crt ? kPhosphor : QColor(),
                                  crt ? blockFor(fc::kIconBlockLogical) : 0);
    fc::setContentTint(crt && phosphorImages ? kPhosphor : QColor());

    // The simulated pixel is defined in logical pixels, so it is scaled here to
    // the image pixels the recolouring code actually works in. Taken from the
    // primary screen: the thumbnail cache stores device-resolution bitmaps
    // marked dpr=1, so there is nothing to derive it from at the point of use
    // (see fc::setContentPixelBlock).
    //
    // Content is quantised only when it is also tinted -- both follow the
    // toggle. Icons are quantised whenever the theme is CRT, on their own
    // coarser grid, because they follow the theme.
    fc::setContentPixelBlock(crt && phosphorImages ? blockFor(fc::kContentBlockLogical) : 0);

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
