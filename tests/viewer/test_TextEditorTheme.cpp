#include <gtest/gtest.h>

#include <QApplication>
#include <QFile>
#include <QHash>
#include <QImage>
#include <QTemporaryDir>
#include <QTextStream>

#include "TextEditor.h"

// The line-number gutter is painted by hand, so the only honest check that a
// theme reached it is the pixels that came out. Every assertion below reads a
// grab of the gutter widget, never the stylesheet text.
namespace {

struct GutterColors {
    QColor background;
    QColor foreground;
    QColor border;
};

// Matches the CodeEditor blocks in resources/themes/*.qss. Kept here rather
// than parsed out of the sheet on purpose: a test that reads its expectations
// from the file under test cannot fail when that file is wrong.
const GutterColors kLight{QColor(0xec, 0xec, 0xec), QColor(0x90, 0x90, 0x90),
                          QColor(0xd0, 0xd0, 0xd0)};
const GutterColors kDark{QColor(0x23, 0x23, 0x23), QColor(0x80, 0x80, 0x80),
                         QColor(0x50, 0x50, 0x50)};
const GutterColors kCrt{QColor(0x0a, 0x1a, 0x0d), QColor(0x1f, 0xa8, 0x5c),
                        QColor(0x12, 0x60, 0x2f)};

void applyThemeSheet(const QString &name) {
    QFile file(QStringLiteral(TTC_SOURCE_DIR "/resources/themes/") + name + QStringLiteral(".qss"));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text)) << qPrintable(name);
    qApp->setStyleSheet(QString::fromUtf8(file.readAll()));
    qApp->processEvents();
}

QString writeSampleFile(const QTemporaryDir &dir, const QString &name) {
    const QString path = dir.filePath(name);
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::WriteOnly));
    // Enough lines that the gutter has real content to paint.
    for (int i = 1; i <= 40; ++i)
        file.write(QByteArray("line ") + QByteArray::number(i) + "\n");
    file.close();
    return path;
}

QImage grabGutter(TextEditor &editor) {
    editor.resize(700, 480);
    editor.show();
    qApp->processEvents();
    QWidget *gutter = editor.codeEditor()->lineNumberArea();
    EXPECT_GT(gutter->width(), 4);
    EXPECT_GT(gutter->height(), 40);
    return gutter->grab().toImage();
}

// The colour covering the largest share of the gutter, ignoring the one-pixel
// rule on its inner edge and the columns the digits are drawn in.
QColor dominantColor(const QImage &image, const QRect &area) {
    QHash<QRgb, int> counts;
    for (int y = area.top(); y <= area.bottom(); ++y)
        for (int x = area.left(); x <= area.right(); ++x)
            ++counts[image.pixel(x, y)];
    QRgb best = 0;
    int bestCount = -1;
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
        if (it.value() > bestCount) {
            bestCount = it.value();
            best = it.key();
        }
    }
    return QColor(best);
}

bool nearColor(const QColor &a, const QColor &b, int tolerance = 8) {
    return qAbs(a.red() - b.red()) <= tolerance && qAbs(a.green() - b.green()) <= tolerance &&
           qAbs(a.blue() - b.blue()) <= tolerance;
}

bool columnContains(const QImage &image, int x, const QColor &color, int tolerance = 8) {
    for (int y = 0; y < image.height(); ++y) {
        if (nearColor(image.pixelColor(x, y), color, tolerance))
            return true;
    }
    return false;
}

// Does any pixel in the gutter carry the number colour? On the Windows
// offscreen platform glyph rasterisation into QWidget::grab() is not
// dependable (see tests/ui/test_TitleBarTheme.cpp), so the caller decides
// whether a false result is a failure.
bool containsGlyphColor(const QImage &image, const QColor &color) {
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width() - 1; ++x) // skip the border column
            if (nearColor(image.pixelColor(x, y), color, 40))
                return true;
    return false;
}

void expectGutterPaintedAs(TextEditor &editor, const GutterColors &expected,
                           const char *themeName) {
    const QImage image = grabGutter(editor);
    ASSERT_FALSE(image.isNull()) << themeName;

    // The fill: everything except the rightmost column (the rule) and the right
    // margin the digits are right-aligned into.
    const QRect fill(0, 0, qMax(1, image.width() - 8), image.height());
    const QColor background = dominantColor(image, fill);
    EXPECT_TRUE(nearColor(background, expected.background))
        << themeName << " gutter background painted " << qPrintable(background.name())
        << ", expected " << qPrintable(expected.background.name());

    // The bug this whole file exists for: a near-white strip. Assert it
    // directly as well, so a future theme that merely gets the wrong dark
    // colour is a different failure from one that regresses to white.
    if (expected.background.lightness() < 128) {
        EXPECT_LT(background.lightness(), 128)
            << themeName << " gutter is light on a dark editor";
    }

    EXPECT_TRUE(columnContains(image, image.width() - 1, expected.border))
        << themeName << " gutter border rule missing from the inner edge";

    EXPECT_EQ(editor.codeEditor()->effectiveGutterForeground(), expected.foreground) << themeName;
#ifndef Q_OS_WIN
    EXPECT_TRUE(containsGlyphColor(image, expected.foreground))
        << themeName << " line numbers are not drawn in the themed colour";
#endif
}

} // namespace

TEST(TextEditorThemeTest, GutterFollowsAllThreeThemes) {
    const QString originalSheet = qApp->styleSheet();
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeSampleFile(dir, QStringLiteral("theme.txt"));

    TextEditor editor;
    ASSERT_TRUE(editor.loadFile(path));

    applyThemeSheet(QStringLiteral("light"));
    expectGutterPaintedAs(editor, kLight, "light");

    applyThemeSheet(QStringLiteral("dark"));
    expectGutterPaintedAs(editor, kDark, "dark");

    applyThemeSheet(QStringLiteral("green"));
    expectGutterPaintedAs(editor, kCrt, "green");

    // And back, so a theme that only ever gets applied second is not the reason
    // it looked right.
    applyThemeSheet(QStringLiteral("light"));
    expectGutterPaintedAs(editor, kLight, "light (returned)");

    qApp->setStyleSheet(originalSheet);
}

TEST(TextEditorThemeTest, EditorCreatedUnderACrtSheetIsAlreadyThemed) {
    // The other test applies themes to a window that already exists. A window
    // opened while the sheet is live takes a different path (polish at
    // construction, not a StyleChange event) and has to end up in the same place.
    const QString originalSheet = qApp->styleSheet();
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeSampleFile(dir, QStringLiteral("crt.txt"));

    applyThemeSheet(QStringLiteral("green"));
    TextEditor editor;
    ASSERT_TRUE(editor.loadFile(path));
    expectGutterPaintedAs(editor, kCrt, "green (constructed under sheet)");

    qApp->setStyleSheet(originalSheet);
}

TEST(TextEditorThemeTest, UnthemedGutterFollowsTheEditorsOwnPaletteNotTheAppPalette) {
    // The safety net under the qproperty hooks. With no sheet at all, the
    // gutter must still be derived from the editor's own Base/Text -- the old
    // code read the APPLICATION palette's AlternateBase, which is exactly how a
    // dark editor ended up with a white strip beside it.
    const QString originalSheet = qApp->styleSheet();
    qApp->setStyleSheet(QString());

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeSampleFile(dir, QStringLiteral("nosheet.txt"));

    TextEditor editor;
    ASSERT_TRUE(editor.loadFile(path));

    QPalette dark = editor.codeEditor()->palette();
    dark.setColor(QPalette::Base, QColor(0x10, 0x10, 0x10));
    dark.setColor(QPalette::Text, QColor(0xf0, 0xf0, 0xf0));
    dark.setColor(QPalette::AlternateBase, QColor(0xff, 0xff, 0xff)); // the old source of truth
    editor.codeEditor()->setPalette(dark);
    qApp->processEvents();

    const QImage image = grabGutter(editor);
    const QColor background = dominantColor(image, QRect(0, 0, qMax(1, image.width() - 8),
                                                          image.height()));
    EXPECT_LT(background.lightness(), 80)
        << "unthemed gutter is " << qPrintable(background.name())
        << " beside a near-black editor";

    qApp->setStyleSheet(originalSheet);
}
