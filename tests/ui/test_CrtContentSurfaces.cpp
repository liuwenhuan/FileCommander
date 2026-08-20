#include <gtest/gtest.h>

#include <QApplication>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QTest>

#include "AppIcon.h"
#include "TitleBar.h"
#include "CommandBar.h"
#include "FileListView.h"
#include "IconFileView.h"
#include "QuickView.h"
#include "ThumbnailDelegate.h"
#include "filesystem/IconCache.h"
#include "theme/Phosphor.h"
#include "theme/ThemeManager.h"
#include "config/Settings.h"
#include "ThemeStateGuard.h"

namespace {

class StyleSheetRestore {
public:
    StyleSheetRestore() : value(qApp->styleSheet()) {}
    ~StyleSheetRestore() { qApp->setStyleSheet(value); }

private:
    QString value;
};

void applyTheme(const QString &name) {
    QFile file(QStringLiteral(TTC_SOURCE_DIR "/resources/themes/") + name +
               QStringLiteral(".qss"));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text));
    qApp->setStyleSheet(QString::fromUtf8(file.readAll()));
    qApp->processEvents();
}

bool containsTileColours(const QImage &image, const QRect &logicalRect,
                         const QList<QColor> &colours, int tolerance = 2) {
    if (image.isNull() || colours.isEmpty())
        return false;

    const qreal dpr = image.devicePixelRatio();
    const QRect rect(qRound(logicalRect.x() * dpr), qRound(logicalRect.y() * dpr),
                     qRound(logicalRect.width() * dpr),
                     qRound(logicalRect.height() * dpr));
    for (const QColor &expected : colours) {
        int matches = 0;
        for (int y = qMax(0, rect.top()); y <= qMin(image.height() - 1, rect.bottom()); ++y) {
            for (int x = qMax(0, rect.left()); x <= qMin(image.width() - 1, rect.right()); ++x) {
                const QColor pixel = image.pixelColor(x, y);
                if (qAbs(pixel.red() - expected.red()) <= tolerance &&
                    qAbs(pixel.green() - expected.green()) <= tolerance &&
                    qAbs(pixel.blue() - expected.blue()) <= tolerance)
                    ++matches;
            }
        }
        if (matches < qMax(4, rect.width()))
            return false;
    }
    return true;
}

bool containsColourNear(const QImage &image, const QRect &logicalRect,
                        const QColor &expected, int tolerance = 12) {
    const qreal dpr = image.devicePixelRatio();
    const QRect rect(qRound(logicalRect.x() * dpr), qRound(logicalRect.y() * dpr),
                     qRound(logicalRect.width() * dpr),
                     qRound(logicalRect.height() * dpr));
    for (int y = qMax(0, rect.top()); y <= qMin(image.height() - 1, rect.bottom()); ++y) {
        for (int x = qMax(0, rect.left()); x <= qMin(image.width() - 1, rect.right()); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (qAbs(pixel.red() - expected.red()) <= tolerance &&
                qAbs(pixel.green() - expected.green()) <= tolerance &&
                qAbs(pixel.blue() - expected.blue()) <= tolerance)
                return true;
        }
    }
    return false;
}

int changedPixelCount(const QImage &before, const QImage &after, const QRect &logicalRect,
                      int tolerance = 2) {
    const qreal dpr = after.devicePixelRatio();
    const QRect rect(qRound(logicalRect.x() * dpr), qRound(logicalRect.y() * dpr),
                     qRound(logicalRect.width() * dpr),
                     qRound(logicalRect.height() * dpr));
    int changed = 0;
    for (int y = qMax(0, rect.top()); y <= qMin(after.height() - 1, rect.bottom()); ++y) {
        for (int x = qMax(0, rect.left()); x <= qMin(after.width() - 1, rect.right()); ++x) {
            const QColor a = before.pixelColor(x, y);
            const QColor b = after.pixelColor(x, y);
            if (qAbs(a.red() - b.red()) > tolerance ||
                qAbs(a.green() - b.green()) > tolerance ||
                qAbs(a.blue() - b.blue()) > tolerance)
                ++changed;
        }
    }
    return changed;
}

const QList<QColor> kScreenTile = {
    QColor(QStringLiteral("#040b05")), QColor(QStringLiteral("#071108")),
    QColor(QStringLiteral("#060e07"))};
const QList<QColor> kSunkenTile = {
    QColor(QStringLiteral("#020c06")), QColor(QStringLiteral("#04140a")),
    QColor(QStringLiteral("#031108"))};
const QList<QColor> kChromeTile = {
    QColor(QStringLiteral("#061008")), QColor(QStringLiteral("#0a1a0d")),
    QColor(QStringLiteral("#08160b"))};

} // namespace

TEST(CrtContentSurfacesTest, QuickViewDefaultPageShowsContinuousScreenScanlines) {
    ThemeStateGuard themeState;
    StyleSheetRestore restore;
    applyTheme(QStringLiteral("green"));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Settings settings(dir.filePath(QStringLiteral("settings.ini")));

    QuickView view(settings);
    view.resize(420, 260);
    view.show();
    qApp->processEvents();

    EXPECT_TRUE(containsTileColours(view.grab().toImage(), QRect(12, 12, 100, 100),
                                    kScreenTile));
}

TEST(CrtContentSurfacesTest, DetailAndIconViewportsShowSunkenScanlines) {
    ThemeStateGuard themeState;
    StyleSheetRestore restore;
    applyTheme(QStringLiteral("green"));

    FileListView details;
    details.resize(420, 260);
    details.show();
    IconFileView icons;
    icons.resize(420, 260);
    icons.show();
    qApp->processEvents();

    const QImage detailsImage = details.grab().toImage();
    const QImage iconsImage = icons.grab().toImage();
    EXPECT_TRUE(containsTileColours(detailsImage,
                                    QRect(details.viewport()->geometry().topLeft() + QPoint(20, 20),
                                          QSize(120, 120)), kSunkenTile));
    EXPECT_TRUE(containsTileColours(iconsImage,
                                    QRect(icons.viewport()->geometry().topLeft() + QPoint(20, 20),
                                          QSize(120, 120)), kSunkenTile));
}

TEST(CrtContentSurfacesTest, CommandInputShowsParentScanlinesAndKeepsSelectionReadable) {
    ThemeStateGuard themeState;
    StyleSheetRestore restore;
    applyTheme(QStringLiteral("green"));

    CommandBar bar;
    bar.setDirectory(QStringLiteral("C:/work"));
    bar.resize(720, 42);
    bar.show();
    qApp->processEvents();
    auto *input = bar.findChild<QLineEdit *>();
    ASSERT_NE(input, nullptr);

    const QImage barImage = bar.grab().toImage();
    EXPECT_TRUE(containsTileColours(barImage,
                                    QRect(input->geometry().x() + input->width() / 2,
                                          input->geometry().y() + 3,
                                          qMax(1, input->width() / 2 - 8),
                                          qMax(1, input->height() - 6)),
                                    kChromeTile));
    const qreal dpr = barImage.devicePixelRatio();
    const int parentX = qRound((input->geometry().left() - 2) * dpr);
    const int inputX = qRound((input->geometry().center().x()) * dpr);
    int matchingRows = 0;
    int sampledRows = 0;
    for (int y = input->geometry().top() + 3; y <= input->geometry().bottom() - 3; ++y) {
        const int deviceY = qRound(y * dpr);
        if (!barImage.rect().contains(parentX, deviceY) ||
            !barImage.rect().contains(inputX, deviceY))
            continue;
        ++sampledRows;
        if (barImage.pixelColor(parentX, deviceY) == barImage.pixelColor(inputX, deviceY))
            ++matchingRows;
    }
    ASSERT_GT(sampledRows, 0);
    EXPECT_GE(matchingRows, sampledRows * 8 / 10);
    EXPECT_EQ(input->palette().color(QPalette::Highlight), QColor(0x33, 0xff, 0x88));
    EXPECT_EQ(input->palette().color(QPalette::HighlightedText), QColor(0x04, 0x14, 0x0a));
    EXPECT_EQ(input->palette().color(QPalette::Text), QColor(0x33, 0xff, 0x88));
}

TEST(CrtContentSurfacesTest, SelectedRowsRemainOpaqueAndHighContrast) {
    ThemeStateGuard themeState;
    StyleSheetRestore restore;
    applyTheme(QStringLiteral("green"));

    QStandardItemModel model(1, 1);
    model.setData(model.index(0, 0), QStringLiteral("selected item"));
    FileListView details;
    details.setModel(&model);
    details.resize(420, 180);
    details.show();
    qApp->processEvents();
    const QRect normalVisual = details.visualRect(model.index(0, 0));
    ASSERT_FALSE(normalVisual.isEmpty());
    const QImage normalImage = details.grab().toImage();
    const QRect normalSample(details.viewport()->geometry().topLeft() +
                                 QPoint(qMax(normalVisual.left(), normalVisual.right() - 24),
                                        normalVisual.top() + 2),
                             QSize(20, qMax(1, normalVisual.height() - 4)));
    EXPECT_TRUE(containsTileColours(normalImage, normalSample, kSunkenTile));

    details.selectRow(0);
    qApp->processEvents();

    const QImage row = details.viewport()->grab().toImage();
    const QRect visual = details.visualRect(model.index(0, 0));
    ASSERT_FALSE(visual.isEmpty());
    EXPECT_EQ(row.pixelColor(qMax(visual.left(), 0) + 2, visual.center().y()),
              QColor(0x33, 0xff, 0x88));
    EXPECT_EQ(details.palette().color(QPalette::HighlightedText), QColor(0x04, 0x14, 0x0a));
}

TEST(CrtContentSurfacesTest, ThumbnailSelectionAndHoverRemainVisibleOverScanlines) {
    ThemeStateGuard themeState;
    StyleSheetRestore restore;
    applyTheme(QStringLiteral("green"));

    QStandardItemModel model(2, 1);
    model.setData(model.index(0, 0), QStringLiteral("selected thumbnail"));
    model.setData(model.index(1, 0), QStringLiteral("hovered thumbnail"));

    IconFileView icons;
    icons.setModel(&model);
    icons.setViewMode(QListView::IconMode);
    icons.setMouseTracking(true);
    auto *delegate = new ThumbnailDelegate(&icons);
    delegate->setView(&icons);
    delegate->setIconSize(64);
    icons.setItemDelegate(delegate);
    icons.setGridSize(delegate->cellSizeHint(icons.font()));
    icons.resize(420, 260);
    icons.show();
    qApp->processEvents();

    const QModelIndex selectedIndex = model.index(0, 0);
    const QModelIndex hoveredIndex = model.index(1, 0);
    icons.selectionModel()->select(selectedIndex, QItemSelectionModel::ClearAndSelect);
    const QRect hoverRect = icons.visualRect(hoveredIndex);
    ASSERT_FALSE(hoverRect.isEmpty());
    const qreal dpr = icons.devicePixelRatioF();
    const QSize imageSize(qRound(icons.viewport()->width() * dpr),
                          qRound(icons.viewport()->height() * dpr));
    QImage beforeHover(imageSize, QImage::Format_ARGB32_Premultiplied);
    beforeHover.setDevicePixelRatio(dpr);
    beforeHover.fill(Qt::transparent);
    QImage afterHover = beforeHover.copy();
    afterHover.setDevicePixelRatio(dpr);

    QStyleOptionViewItem hoverOption;
    hoverOption.rect = hoverRect;
    hoverOption.state = QStyle::State_Enabled;
    hoverOption.palette = icons.palette();
    hoverOption.font = icons.font();
    hoverOption.fontMetrics = icons.fontMetrics();
    hoverOption.widget = &icons;
    {
        QPainter painter(&beforeHover);
        delegate->paint(&painter, hoverOption, hoveredIndex);
    }
    hoverOption.state |= QStyle::State_MouseOver;
    {
        QPainter painter(&afterHover);
        delegate->paint(&painter, hoverOption, hoveredIndex);
    }

    const QRect selectedRect = icons.visualRect(selectedIndex);
    ASSERT_FALSE(selectedRect.isEmpty());
    const QImage selectedImage = icons.viewport()->grab().toImage();
    EXPECT_TRUE(containsColourNear(selectedImage, selectedRect.adjusted(2, 2, -2, -2),
                                   QColor(0x33, 0xff, 0x88)));
    EXPECT_GT(changedPixelCount(beforeHover, afterHover, hoverRect.adjusted(4, 4, -4, -4)),
              40);
}

TEST(CrtContentSurfacesTest, LightAndDarkSurfacesDoNotAcquireCrtTiles) {
    ThemeStateGuard themeState;
    StyleSheetRestore restore;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Settings settings(dir.filePath(QStringLiteral("settings.ini")));
    for (const QString &theme : {QStringLiteral("light"), QStringLiteral("dark")}) {
        applyTheme(theme);

        QStandardItemModel model(1, 1);
        model.setData(model.index(0, 0), QStringLiteral("ordinary row"));
        FileListView details;
        details.setModel(&model);
        details.resize(360, 220);
        details.show();
        IconFileView icons;
        icons.resize(360, 220);
        icons.show();
        CommandBar bar;
        bar.resize(620, 42);
        bar.show();
        QuickView quickView(settings);
        quickView.resize(360, 220);
        quickView.show();
        qApp->processEvents();
        auto *input = bar.findChild<QLineEdit *>();
        ASSERT_NE(input, nullptr);

        EXPECT_FALSE(containsTileColours(details.viewport()->grab().toImage(),
                                         QRect(20, 20, 100, 100), kSunkenTile))
            << theme.toStdString();
        EXPECT_FALSE(containsTileColours(input->grab().toImage(),
                                         QRect(input->width() / 2, 3,
                                               qMax(1, input->width() / 2 - 8),
                                               qMax(1, input->height() - 6)),
                                         kChromeTile))
            << theme.toStdString();
        EXPECT_FALSE(containsTileColours(icons.grab().toImage(),
                                         QRect(20, 20, 100, 100), kSunkenTile))
            << theme.toStdString();
        EXPECT_FALSE(containsTileColours(quickView.grab().toImage(),
                                         QRect(12, 12, 100, 100), kScreenTile))
            << theme.toStdString();

        const QRect visual = details.visualRect(model.index(0, 0));
        ASSERT_FALSE(visual.isEmpty());
        const QImage detailsImage = details.grab().toImage();
        const QRect ordinaryRow(details.viewport()->geometry().topLeft() +
                                    visual.topLeft(),
                                visual.size());
        EXPECT_TRUE(containsColourNear(detailsImage, ordinaryRow.adjusted(5, 2, -5, -2),
                                       details.palette().color(QPalette::Base), 2))
            << theme.toStdString();
    }
}

// The two content switches must reach two different surfaces. They were one
// setting until it became clear they answer different questions: recolouring a
// wall of small thumbnails is decoration, while recolouring the picture someone
// opened to LOOK at changes what they are examining.
TEST(CrtContentSurfacesTest, TheImageAndPreviewSwitchesAreIndependent) {
    ThemeStateGuard themeState;
    ThemeManager manager;
    struct Restore {
        ~Restore() {
            fc::setThumbnailTint(QColor());
            fc::setPreviewTint(QColor());
            IconCache::instance().setFileIconTint(QColor());
        }
    } restore;

    // Images on, preview off: the grid follows the theme, the preview does not.
    manager.apply(Settings::Theme::Crt, true, false);
    EXPECT_TRUE(fc::thumbnailTint().isValid());
    EXPECT_TRUE(IconCache::instance().fileIconTint().isValid())
        << "file-type icons share the grid with thumbnails and the switch with them";
    EXPECT_FALSE(fc::previewTint().isValid())
        << "the preview pane followed the images switch";

    // ...and the other way round.
    manager.apply(Settings::Theme::Crt, false, true);
    EXPECT_FALSE(fc::thumbnailTint().isValid());
    EXPECT_FALSE(IconCache::instance().fileIconTint().isValid());
    EXPECT_TRUE(fc::previewTint().isValid());

    // Off means off under every theme.
    manager.apply(Settings::Theme::Light, false, false);
    EXPECT_FALSE(fc::thumbnailTint().isValid());
    EXPECT_FALSE(fc::previewTint().isValid());
    EXPECT_FALSE(IconCache::instance().fileIconTint().isValid());
}

// The mapping is luma -> tint * k, so the tint is literally what WHITE becomes.
// A dark one therefore does not "tint" an image, it darkens it: the light
// theme's chrome colour (#404040) turned every folder, globe and document into
// a grey slab. Whatever palette a theme picks from, the content colour has to
// be a bright member of it.
TEST(CrtContentSurfacesTest, EveryThemesContentTintIsBrightEnoughToTintRatherThanDarken) {
    ThemeStateGuard themeState;
    ThemeManager manager;
    struct Restore {
        ~Restore() {
            fc::setThumbnailTint(QColor());
            fc::setPreviewTint(QColor());
            IconCache::instance().setFileIconTint(QColor());
        }
    } restore;

    for (Settings::Theme theme : {Settings::Theme::Crt, Settings::Theme::Dark}) {
        manager.apply(theme, true, true);
        const QColor tint = fc::thumbnailTint();
        ASSERT_TRUE(tint.isValid()) << "theme " << int(theme);
        EXPECT_EQ(tint, fc::previewTint()) << "both surfaces recolour to the same hue";
        EXPECT_EQ(tint, IconCache::instance().fileIconTint());
        // Anything below this and white maps to something a viewer reads as
        // "the picture went dark" rather than "the picture is tinted".
        EXPECT_GE(tint.lightness(), 140)
            << "theme " << int(theme) << " content tint " << tint.name().toStdString()
            << " is what white becomes -- too dark to be a tint";
    }
}

// ...and the light theme is the case where no bright colour exists to pick. On
// a white page a bright hue over full-colour artwork is a colour cast, not a
// duotone: the #9cc0f0 tried here was reported as a dead blue film over every
// icon in the file list. Darker brings back the grey slab above, whiter erases
// the icons into the page, so the light theme recolours nothing -- with BOTH
// switches on. (Auto resolves to this same branch when the desktop is light.)
TEST(CrtContentSurfacesTest, TheLightThemeForcesNoColourOntoContent) {
    ThemeStateGuard themeState;
    ThemeManager manager;
    struct Restore {
        ~Restore() {
            fc::setThumbnailTint(QColor());
            fc::setPreviewTint(QColor());
            IconCache::instance().setFileIconTint(QColor());
        }
    } restore;

    // Seed a tint a bug could leave standing, so "no tint" cannot pass by
    // nothing having been set in the first place.
    manager.apply(Settings::Theme::Crt, true, true);
    ASSERT_TRUE(fc::thumbnailTint().isValid());

    manager.apply(Settings::Theme::Light, true, true);
    EXPECT_FALSE(manager.contentTint().isValid())
        << "light content tint " << manager.contentTint().name().toStdString();
    EXPECT_FALSE(fc::thumbnailTint().isValid()) << "thumbnails took a cast";
    EXPECT_FALSE(IconCache::instance().fileIconTint().isValid())
        << "file-type icons took a cast";
    EXPECT_FALSE(fc::previewTint().isValid()) << "the preview pane took a cast";

    // The chrome glyphs are a separate decision and must NOT have gone with it:
    // they are monochrome line art, and flattening them to the theme's text
    // colour is what keeps them at the weight of the labels beside them.
    EXPECT_TRUE(IconCache::instance().glyphTint().isValid());
}

// The dark theme's accent must not be the one saturated thing in an otherwise
// greyscale grid. Once the file list follows the theme, its icons and
// thumbnails are #e0e0e0 monochrome, and the old #3d7deb selection tile and
// up-arrow stood out as leftovers rather than as accents.
TEST(CrtContentSurfacesTest, TheDarkThemesAccentSitsWithItsGreyscaleContent) {
    ThemeStateGuard themeState;
    ThemeManager manager;
    // Put the sheet back rather than blanking it: other suites in this binary
    // are written against whatever was applied before, and clearing it left
    // them painting with no theme at all.
    struct Restore {
        QString sheet;
        ~Restore() {
            fc::setThumbnailTint(QColor());
            fc::setPreviewTint(QColor());
            IconCache::instance().setFileIconTint(QColor());
            qApp->setStyleSheet(sheet);
        }
    } restore{qApp->styleSheet()};

    manager.apply(Settings::Theme::Dark, true, true);

    // Read the accent the way the painting code does -- off a widget's palette,
    // which the stylesheet's selection-background-color fills in.
    QWidget probe;
    probe.setAttribute(Qt::WA_StyledBackground, true);
    probe.ensurePolished();
    const QColor accent = probe.palette().color(QPalette::Highlight);
    const QColor content = fc::thumbnailTint();
    ASSERT_TRUE(content.isValid());

    EXPECT_LE(accent.saturation(), 90)
        << "accent " << accent.name().toStdString() << " is saturated next to "
        << content.name().toStdString() << " content";
    // ...and still distinguishable from the surface it sits on, or it stops
    // being an accent at all.
    EXPECT_GE(accent.lightness(), 48);
}

// The app icon sits in the title bar beside the themed chrome glyphs. Leaving
// it the stock blue made it the one thing in the window that had not noticed
// the theme -- and it is painted by us, through the same recolouring path, so
// there was never a reason for it to be exempt.
//
// Read from the title bar rather than from qApp->windowIcon(). Those are two
// different surfaces: the window icon is what the DESKTOP draws, on a
// background it chooses, and tinting that one is what made the mark vanish
// against a dark taskbar. This asserts the themed mark reaches the bar we
// paint; TitleBarThemeTest asserts the window icon stays the brand mark.
TEST(CrtContentSurfacesTest, TheAppIconTakesTheSameColourAsTheChromeGlyphs) {
    ThemeStateGuard themeState;
    ThemeManager manager;
    struct Restore {
        QString sheet;
        ~Restore() { qApp->setStyleSheet(sheet); }
    } restore{qApp->styleSheet()};

    // Dominant colour of the icon at title-bar size, ignoring what the shape
    // leaves transparent.
    auto dominant = [](const QIcon &icon) {
        const QImage image = icon.pixmap(32, 32).toImage();
        qint64 r = 0, g = 0, b = 0, n = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const QColor pixel = image.pixelColor(x, y);
                if (pixel.alpha() < 200)
                    continue;
                r += pixel.red();
                g += pixel.green();
                b += pixel.blue();
                ++n;
            }
        }
        return n ? QColor(int(r / n), int(g / n), int(b / n)) : QColor();
    };

    const QColor stock = dominant(ttc::appIcon());
    ASSERT_TRUE(stock.isValid());
    EXPECT_GT(stock.saturation(), 60) << "the untinted icon is the blue one";

    QWidget host;
    TitleBar bar(&host, {}, &host);
    QLabel *iconLabel = nullptr;
    for (QLabel *label : bar.findChildren<QLabel *>()) {
        if (label->objectName() == QStringLiteral("ApplicationIcon")) {
            iconLabel = label;
            break;
        }
    }
    ASSERT_NE(iconLabel, nullptr);

    for (Settings::Theme theme : {Settings::Theme::Dark, Settings::Theme::Light,
                                  Settings::Theme::Crt}) {
        manager.apply(theme, true, true);
        ASSERT_NE(iconLabel->pixmap(), nullptr) << "theme " << int(theme);
        QIcon shown;
        shown.addPixmap(*iconLabel->pixmap());
        const QColor themed = dominant(shown);
        ASSERT_TRUE(themed.isValid()) << "theme " << int(theme);
        EXPECT_NE(themed.rgb(), stock.rgb())
            << "theme " << int(theme) << " left the app icon in its stock colours";
        // ...and it landed on the theme's own hue rather than some third one.
        const QColor glyph = IconCache::instance().glyphTint();
        ASSERT_TRUE(glyph.isValid());
        const int hueGap = qAbs(themed.hue() - glyph.hue());
        EXPECT_TRUE(themed.saturation() < 40 || hueGap < 30 || hueGap > 330)
            << "theme " << int(theme) << ": icon " << themed.name().toStdString()
            << " vs glyph colour " << glyph.name().toStdString();
    }
}

// The preview's info overlay was white-on-black in every theme -- the one thing
// in the pane that had not noticed which theme was on. It is a stylesheet, so
// palette(...) is what makes it follow one: Qt re-resolves those when the
// palette changes, with no refresh plumbing on our side.
TEST(CrtContentSurfacesTest, ThePreviewInfoOverlayTakesItsColoursFromTheTheme) {
    ThemeStateGuard themeState;
    StyleSheetRestore restore;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Settings settings(dir.filePath(QStringLiteral("settings.ini")));

    // Compared as PIXELS, and via the theme sheets rather than the widget's own
    // palette. Two earlier drafts measured the wrong thing: reading
    // overlay->palette() never sees a stylesheet at all (#000000 under both
    // themes), and putting palette(...) in an inline sheet resolves against the
    // APPLICATION palette -- which the themes do not touch, since they are
    // applied with qApp->setStyleSheet(). Both "passed" while the overlay was
    // still identical under every theme.
    auto renderUnder = [&](const QString &theme) {
        applyTheme(theme);
        QuickView view(settings);
        view.resize(600, 400);
        view.show();
        qApp->processEvents();

        QLabel *overlay = nullptr;
        for (QLabel *label : view.findChildren<QLabel *>()) {
            if (label->objectName() == QStringLiteral("quickViewInfoOverlay"))
                overlay = label;
        }
        if (!overlay)
            return QImage();
        overlay->setText(QStringLiteral("4000 x 2667"));
        overlay->adjustSize();
        overlay->show();
        qApp->processEvents();
        return overlay->grab().toImage();
    };

    const QImage light = renderUnder(QStringLiteral("light"));
    const QImage crt = renderUnder(QStringLiteral("green"));
    ASSERT_FALSE(light.isNull()) << "no info overlay found";
    ASSERT_FALSE(crt.isNull());
    ASSERT_EQ(light.size(), crt.size());

    EXPECT_NE(light, crt)
        << "the overlay paints identically under the light and CRT themes -- it "
           "is following neither";
}
