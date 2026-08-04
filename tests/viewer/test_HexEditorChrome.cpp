#include <gtest/gtest.h>

#include <QApplication>
#include <QFile>
#include <QFontDatabase>
#include <QImage>

#include "HexEditor.h"

namespace {

QString themeSheet(const char *name) {
    QFile file(QStringLiteral(TTC_SOURCE_DIR "/resources/themes/%1.qss").arg(
        QString::fromLatin1(name)));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

// The colour that covers most of the leftmost column, which is where the
// address strip lives.
QColor addressStripColour(HexEditor &editor) {
    const QImage shot = editor.grab().toImage();
    QHash<QRgb, int> counts;
    for (int y = 4; y < shot.height() - 4; ++y)
        for (int x = 2; x < 20; ++x)
            counts[shot.pixel(x, y)]++;
    QRgb best = 0;
    int most = -1;
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
        if (it.value() > most) {
            most = it.value();
            best = it.key();
        }
    }
    return QColor(best);
}

} // namespace

// A hex dump of a binary file showed a white strip down its left edge in the
// dark and green themes. The address column's default came from
// QPalette::AlternateBase -- a role no theme in this app sets, so it stayed at
// the platform default. The text editor's line-number gutter had exactly this
// bug; this is the same one, one widget over.
//
// Two separate things have to hold, and testing only the first would pass with
// the fallback still broken -- the stylesheet rule hides it.
TEST(HexEditorChrome, TheAddressColumnIsNeverAWhiteStrip) {
    for (const char *theme : {"green", "dark", "light"}) {
        const QString sheet = themeSheet(theme);
        ASSERT_FALSE(sheet.isEmpty()) << theme;
        qApp->setStyleSheet(sheet);

        HexEditor editor;
        editor.resize(600, 300);
        editor.setContents(QByteArray(512, 'A'));
        editor.show();
        qApp->processEvents();

        const QColor strip = addressStripColour(editor);
        const bool dark = QString::fromLatin1(theme) != QLatin1String("light");
        if (dark) {
            EXPECT_LT(strip.lightness(), 128)
                << theme << " address column is " << strip.name().toStdString();
        } else {
            EXPECT_GT(strip.lightness(), 128)
                << theme << " address column is " << strip.name().toStdString();
        }
        editor.hide();
    }
    qApp->setStyleSheet(QString());
}

// The fallback, with NO stylesheet at all: a theme that names no colour of its
// own must still not get a white column on a dark window. This is the half the
// stylesheet rules above would otherwise hide.
TEST(HexEditorChrome, TheAddressColumnFallbackFollowsTheWidgetsOwnPalette) {
    qApp->setStyleSheet(QString());

    HexEditor editor;
    QPalette dark = editor.palette();
    dark.setColor(QPalette::Base, QColor(0x10, 0x10, 0x10));
    dark.setColor(QPalette::Text, QColor(0xe0, 0xe0, 0xe0));
    // The role the old fallback used, left deliberately white: if the code
    // reads it again, this is what appears on screen.
    dark.setColor(QPalette::AlternateBase, QColor(Qt::white));
    editor.setPalette(dark);

    editor.resize(600, 300);
    editor.setContents(QByteArray(512, 'A'));
    editor.show();
    qApp->processEvents();

    const QColor strip = addressStripColour(editor);
    EXPECT_LT(strip.lightness(), 128)
        << "the address column fell back to AlternateBase: " << strip.name().toStdString();
}

// The widget took the system's fixed-pitch font WHOLE, point size included, so
// it ignored the application font the user had configured and came out smaller
// than the text editor beside it.
//
// Tested through fontFor() rather than through the widget, because the platform
// the tests run on cannot reproduce the precondition: offscreen returns a
// substitute whose size is UNRESOLVED and therefore already inherits, so a test
// through the widget passes with the bug present. Confirmed by measurement --
// systemFont(FixedFont) there is "Helvetica" at the same 12 pt as the app font.
TEST(HexEditorChrome, TheFontTakesTheFamilyFromTheSystemAndTheSizeFromTheApp) {
    QFont systemFixed(QStringLiteral("Consolas"));
    systemFixed.setPointSize(9); // resolved, as it is on a real Windows desktop
    QFont inherited(QStringLiteral("Segoe UI"));
    inherited.setPointSize(16);

    const QFont result = HexEditor::fontFor(systemFixed, inherited);
    EXPECT_EQ(result.family(), QStringLiteral("Consolas")) << "columns need a fixed pitch";
    EXPECT_EQ(result.pointSize(), 16) << "the configured size was overridden";
}

// A configuration expressed in pixels must not fall back to the system font's
// own dimensions either.
TEST(HexEditorChrome, APixelSizedConfigurationIsCarriedOverToo) {
    QFont systemFixed(QStringLiteral("Consolas"));
    systemFixed.setPointSize(9);
    QFont inherited(QStringLiteral("Segoe UI"));
    inherited.setPixelSize(24);

    const QFont result = HexEditor::fontFor(systemFixed, inherited);
    EXPECT_EQ(result.family(), QStringLiteral("Consolas"));
    EXPECT_EQ(result.pixelSize(), 24);
}

// Sixteen bytes a row left the right-hand half of the window empty. The row
// grows to what the window can show, in multiples of eight.
TEST(HexEditorChrome, TheRowGrowsToUseTheWindowWidth) {
    HexEditor editor;
    editor.setContents(QByteArray(4096, '\x00'));

    editor.resize(400, 300);
    editor.show();
    qApp->processEvents();
    const int narrow = editor.bytesPerLine();

    editor.resize(1600, 300);
    qApp->processEvents();
    const int wide = editor.bytesPerLine();

    EXPECT_GT(wide, narrow) << "a wider window shows no more bytes per row";
    EXPECT_EQ(wide % 8, 0) << "rows should stay in groups of eight";
    EXPECT_GE(narrow, 8) << "a narrow window must still show a usable row";
}

// ...unless the caller said what it wanted, which is a decision and not a
// starting point.
TEST(HexEditorChrome, AnExplicitRowWidthIsNotOverriddenByAResize) {
    HexEditor editor;
    editor.setContents(QByteArray(4096, '\x00'));
    editor.setBytesPerLine(16);
    editor.resize(1600, 300);
    editor.show();
    qApp->processEvents();
    EXPECT_EQ(editor.bytesPerLine(), 16);
}
