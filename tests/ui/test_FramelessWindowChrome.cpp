#include <gtest/gtest.h>

#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>

#include "DialogTitleBar.h"
#include "FramelessWindow.h"
#include "TextEditor.h"
#include "ViewerWindow.h"
#include "config/Settings.h"
#include "theme/ThemeManager.h"

// The F3 viewer (ViewerWindow) and the F4 editor (TextEditor) used to be plain
// QWidget top-level windows, so the window manager decorated them: a stock
// native title bar sat directly on top of a fully themed body. Both now derive
// from the shared FramelessWindow chrome, and these tests assert the composition
// that makes that true -- the actual title-bar widget, its controls, and the
// colours it paints in each of the three themes -- rather than a window flag.

namespace {

// The app's own stylesheets, read from the source tree: resources.qrc is
// compiled into this binary too, but reading the file keeps a failure pointing
// at the theme that broke.
void applyThemeSheet(const QString &name) {
    QFile file(QStringLiteral(TTC_SOURCE_DIR "/resources/themes/") + name + QStringLiteral(".qss"));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text));
    qApp->setStyleSheet(QString::fromUtf8(file.readAll()));
    qApp->processEvents();
}

QString writeSampleFile(const QTemporaryDir &dir, const QString &name, const QString &body) {
    const QString path = QDir(dir.path()).filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return QString();
    QTextStream(&file) << body;
    file.close();
    return path;
}

// The colour a widget's background actually came out as, sampled below the
// centred title text and inside the rounded corners.
QColor paintedBackground(QWidget &widget) {
    const QImage image = widget.grab().toImage();
    return image.pixelColor(widget.width() / 2, widget.height() - 2);
}

bool nearlyEqual(const QColor &a, const QColor &b, int tolerance = 12) {
    return qAbs(a.red() - b.red()) <= tolerance && qAbs(a.green() - b.green()) <= tolerance &&
           qAbs(a.blue() - b.blue()) <= tolerance;
}

// The title is drawn in the theme's text colour dimmed to alpha 150, over the
// bar's own background -- so this is the colour those glyph pixels land on.
QColor dimmedTitleColor(const QColor &text, const QColor &background) {
    const qreal a = 150.0 / 255.0;
    return QColor(qRound(text.red() * a + background.red() * (1 - a)),
                  qRound(text.green() * a + background.green() * (1 - a)),
                  qRound(text.blue() * a + background.blue() * (1 - a)));
}

int pixelsMatching(const QImage &image, const QColor &wanted, int tolerance) {
    int count = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.alpha() > 0 && nearlyEqual(pixel, wanted, tolerance))
                ++count;
        }
    }
    return count;
}

// Whether drawn text leaves any ink on this platform. The Windows offscreen
// plugin rasterizes shapes but not glyphs (the same limitation test_TitleBarTheme
// works around for tool-button text), so the assertions that read the painted
// TITLE are gated on this rather than on an OS #ifdef -- on a platform that can
// rasterize, they run for real.
bool platformRasterizesText() {
    QImage probe(60, 24, QImage::Format_ARGB32);
    probe.fill(Qt::black);
    QPainter p(&probe);
    p.setPen(Qt::white);
    p.drawText(2, 18, QStringLiteral("Xg"));
    p.end();
    for (int y = 0; y < probe.height(); ++y)
        for (int x = 0; x < probe.width(); ++x)
            if (probe.pixelColor(x, y) != QColor(Qt::black))
                return true;
    return false;
}

// Restores whatever stylesheet / window icon the rest of the suite was using.
class AppChromeRestore final {
public:
    AppChromeRestore() : m_sheet(qApp->styleSheet()), m_icon(qApp->windowIcon()) {}
    ~AppChromeRestore() {
        qApp->setStyleSheet(m_sheet);
        qApp->setWindowIcon(m_icon);
    }

private:
    QString m_sheet;
    QIcon m_icon;
};

} // namespace

TEST(FramelessWindowChromeTest, ViewerWindowCarriesTheSharedTitleBar) {
    AppChromeRestore restore;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeSampleFile(dir, QStringLiteral("note.txt"), QStringLiteral("hello"));
    ASSERT_FALSE(path.isEmpty());
    Settings settings(QDir(dir.path()).filePath(QStringLiteral("settings.ini")));

    QPointer<ViewerWindow> viewer(new ViewerWindow(settings, path));
    viewer->show();
    qApp->processEvents();

    // The window is the app's own chrome, not the window manager's.
    EXPECT_NE(qobject_cast<FramelessWindow *>(viewer.data()), nullptr);
    EXPECT_TRUE(viewer->windowFlags().testFlag(Qt::FramelessWindowHint));

    DialogTitleBar *bar = viewer->findChild<DialogTitleBar *>();
    ASSERT_NE(bar, nullptr) << "the F3 viewer is back on the native window frame";
    EXPECT_EQ(bar->controls(), DialogTitleBar::WindowControls);
    EXPECT_EQ(viewer->titleBar(), bar);
    // It sits along the top of the window, spanning it.
    EXPECT_GT(bar->height(), 0);
    EXPECT_GT(bar->width(), viewer->width() / 2);
    EXPECT_LT(bar->geometry().top(), bar->height());
    // Content starts below it, so the bar can never overlap the preview.
    EXPECT_GE(viewer->contentsRect().top(), bar->geometry().bottom());

    delete viewer.data();
}

TEST(FramelessWindowChromeTest, TextEditorCarriesTheSharedTitleBar) {
    AppChromeRestore restore;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeSampleFile(dir, QStringLiteral("edit.txt"), QStringLiteral("abc"));
    ASSERT_FALSE(path.isEmpty());

    QPointer<TextEditor> editor(new TextEditor);
    ASSERT_TRUE(editor->loadFile(path));
    editor->resize(600, 400);
    editor->show();
    qApp->processEvents();

    EXPECT_NE(qobject_cast<FramelessWindow *>(editor.data()), nullptr);
    EXPECT_TRUE(editor->windowFlags().testFlag(Qt::FramelessWindowHint));

    DialogTitleBar *bar = editor->findChild<DialogTitleBar *>();
    ASSERT_NE(bar, nullptr) << "the F4 editor is back on the native window frame";
    EXPECT_EQ(bar->controls(), DialogTitleBar::WindowControls);
    EXPECT_GE(editor->contentsRect().top(), bar->geometry().bottom());

    delete editor.data();
}

// A window manager gives a decorated window a taskbar / alt-tab entry, a window
// menu, and (on Windows) the snap gestures, all of which follow from the window
// TYPE and the minimize / maximize hints -- not from the decorations. Dropping
// the decorations must not drop those.
TEST(FramelessWindowChromeTest, BothWindowsStayOrdinaryManagedWindows) {
    AppChromeRestore restore;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeSampleFile(dir, QStringLiteral("note.txt"), QStringLiteral("hello"));
    ASSERT_FALSE(path.isEmpty());
    Settings settings(QDir(dir.path()).filePath(QStringLiteral("settings.ini")));

    QPointer<ViewerWindow> viewer(new ViewerWindow(settings, path));
    QPointer<TextEditor> editor(new TextEditor);
    for (QWidget *w : {static_cast<QWidget *>(viewer.data()), static_cast<QWidget *>(editor.data())}) {
        w->show();
        qApp->processEvents();
        EXPECT_TRUE(w->isWindow());
        // Qt::Window, not Qt::Tool / Qt::Popup: a tool window is kept out of the
        // taskbar and the alt-tab list.
        EXPECT_EQ(w->windowFlags() & Qt::WindowType_Mask, Qt::Window);
        EXPECT_TRUE(w->windowFlags().testFlag(Qt::WindowMinimizeButtonHint));
        EXPECT_TRUE(w->windowFlags().testFlag(Qt::WindowMaximizeButtonHint));
        EXPECT_TRUE(w->windowFlags().testFlag(Qt::WindowCloseButtonHint));
    }

    delete viewer.data();
    delete editor.data();
}

TEST(FramelessWindowChromeTest, TitleBarKeepsMinimizeMaximizeAndClose) {
    AppChromeRestore restore;
    QPointer<TextEditor> editor(new TextEditor);
    editor->setWindowTitle(QStringLiteral("edit.txt"));
    editor->resize(600, 400);
    editor->show();
    qApp->processEvents();

    DialogTitleBar *bar = editor->findChild<DialogTitleBar *>();
    ASSERT_NE(bar, nullptr);
    const QList<QAbstractButton *> buttons = bar->findChildren<QAbstractButton *>();
    ASSERT_EQ(buttons.size(), 3) << "expected minimize, maximize and close";

    // Double-clicking the bar toggles maximize, the way a decorated window does.
    const QPoint middle(bar->width() / 2, bar->height() / 2);
    QTest::mouseDClick(bar, Qt::LeftButton, Qt::KeyboardModifiers(), middle);
    qApp->processEvents();
    EXPECT_TRUE(editor->isMaximized());
    // Maximized: the shadow margin is collapsed so no transparent gap shows at
    // the screen edges, and the bar still spans the window.
    EXPECT_EQ(editor->contentsMargins().left(), 0);
    EXPECT_EQ(bar->width(), editor->width());

    QTest::mouseDClick(bar, Qt::LeftButton, Qt::KeyboardModifiers(), middle);
    qApp->processEvents();
    EXPECT_FALSE(editor->isMaximized());
    EXPECT_GT(editor->contentsMargins().left(), 0);

    // The close button closes the window (WA_DeleteOnClose then disposes of it).
    buttons.last()->click();
    qApp->processEvents();
    qApp->sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_TRUE(editor.isNull());
}

TEST(FramelessWindowChromeTest, ViewerTitleBarColoursTrackEveryTheme) {
    AppChromeRestore restore;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeSampleFile(dir, QStringLiteral("note.txt"), QStringLiteral("hello"));
    ASSERT_FALSE(path.isEmpty());
    Settings settings(QDir(dir.path()).filePath(QStringLiteral("settings.ini")));

    QPointer<ViewerWindow> viewer(new ViewerWindow(settings, path));
    viewer->setWindowTitle(QStringLiteral("note.txt"));
    viewer->show();
    qApp->processEvents();
    DialogTitleBar *bar = viewer->findChild<DialogTitleBar *>();
    ASSERT_NE(bar, nullptr);

    struct Case {
        const char *sheet;
        QColor window;
        QColor text;
    };
    // The window / text colours each theme's own QWidget rule declares.
    const Case cases[] = {
        {"light", QColor(0xf5, 0xf5, 0xf5), QColor(0x20, 0x20, 0x20)},
        {"dark", QColor(0x2b, 0x2b, 0x2b), QColor(0xe0, 0xe0, 0xe0)},
        {"green", QColor(0x07, 0x11, 0x08), QColor(0x33, 0xff, 0x88)},
    };

    for (const Case &c : cases) {
        SCOPED_TRACE(c.sheet);
        applyThemeSheet(QString::fromLatin1(c.sheet));
        qApp->processEvents();

        EXPECT_EQ(bar->palette().color(QPalette::WindowText), c.text);

        // The CRT theme hands chrome a scanline tile instead of a flat colour,
        // through the same qproperty the main window's TitleBar uses.
        const bool crt = QLatin1String(c.sheet) == QLatin1String("green");
        EXPECT_EQ(!bar->backgroundTile().isNull(), crt)
            << "backgroundTile does not follow the theme";

        // The tile is a 1x3 scanline strip repeated from the widget's own
        // top-left, so the sampled row picks the strip row it landed on.
        QColor expectedBackground = c.window;
        if (crt) {
            const QImage tile =
                QPixmap(QStringLiteral(":/icons/crt-scan-chrome.png")).toImage();
            ASSERT_FALSE(tile.isNull());
            expectedBackground = tile.pixelColor(0, (bar->height() - 2) % tile.height());
        }
        EXPECT_TRUE(nearlyEqual(paintedBackground(*bar), expectedBackground))
            << "painted " << qPrintable(paintedBackground(*bar).name()) << ", wanted "
            << qPrintable(expectedBackground.name());

        // The window buttons paint their own glyphs (so the platform style can
        // never recolour them) in the theme's text colour.
        const QList<QAbstractButton *> buttons = bar->findChildren<QAbstractButton *>();
        ASSERT_EQ(buttons.size(), 3);
        EXPECT_GT(pixelsMatching(buttons.last()->grab().toImage(), c.text, 40), 0)
            << "the close glyph is not in the theme's text colour";

        // And the title itself, dimmed to read as chrome.
        if (platformRasterizesText()) {
            const QImage rendered = bar->grab().toImage();
            const QRect titleArea(90, 0, rendered.width() - 90 - 3 * 46, rendered.height());
            EXPECT_GT(pixelsMatching(rendered.copy(titleArea),
                                     dimmedTitleColor(c.text, expectedBackground), 40),
                      5)
                << "no title text in the theme's colour";
        }
    }

    delete viewer.data();
}

TEST(FramelessWindowChromeTest, CrtTileReachesTheViewerAndIsClearedOnTheWayOut) {
    AppChromeRestore restore;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeSampleFile(dir, QStringLiteral("note.txt"), QStringLiteral("hello"));
    ASSERT_FALSE(path.isEmpty());
    Settings settings(QDir(dir.path()).filePath(QStringLiteral("settings.ini")));

    QPointer<ViewerWindow> viewer(new ViewerWindow(settings, path));
    viewer->show();
    qApp->processEvents();
    DialogTitleBar *bar = viewer->findChild<DialogTitleBar *>();
    ASSERT_NE(bar, nullptr);

    ThemeManager themeManager;
    themeManager.apply(Settings::Theme::Crt);
    qApp->processEvents();
    ASSERT_FALSE(viewer->backgroundTile().isNull());
    EXPECT_FALSE(bar->backgroundTile().isNull());

    // Leaving CRT has to clear the tile explicitly: a stylesheet cannot unset a
    // qproperty it no longer mentions.
    themeManager.apply(Settings::Theme::Dark);
    qApp->processEvents();
    EXPECT_TRUE(viewer->backgroundTile().isNull());
    EXPECT_TRUE(bar->backgroundTile().isNull());

    themeManager.apply(Settings::Theme::Crt);
    qApp->processEvents();
    ASSERT_FALSE(viewer->backgroundTile().isNull());
    themeManager.apply(Settings::Theme::Light);
    qApp->processEvents();
    EXPECT_TRUE(viewer->backgroundTile().isNull());
    EXPECT_TRUE(bar->backgroundTile().isNull());

    delete viewer.data();
}

// The editor appends " *" to its window title while the document is modified.
// The bar reads the title live at paint time, so that marker only ever appears
// if a title change also makes it repaint.
TEST(FramelessWindowChromeTest, TitleBarRepaintsWhenTheWindowTitleChanges) {
    AppChromeRestore restore;
    applyThemeSheet(QStringLiteral("dark"));

    QPointer<TextEditor> editor(new TextEditor);
    editor->resize(600, 400);
    editor->setWindowTitle(QStringLiteral("edit.txt"));
    editor->show();
    qApp->processEvents();
    DialogTitleBar *bar = editor->findChild<DialogTitleBar *>();
    ASSERT_NE(bar, nullptr);

    class PaintCounter : public QObject {
    public:
        int paints = 0;

    protected:
        bool eventFilter(QObject *, QEvent *event) override {
            if (event->type() == QEvent::Paint)
                ++paints;
            return false;
        }
    } counter;
    const QImage before = bar->grab().toImage();
    qApp->processEvents(); // drain anything already pending
    bar->installEventFilter(&counter);
    counter.paints = 0;

    editor->setWindowTitle(QStringLiteral("edit.txt *"));
    qApp->processEvents();
    EXPECT_GT(counter.paints, 0) << "the title bar was not repainted for the new title";
    bar->removeEventFilter(&counter);

    if (platformRasterizesText()) {
        const QImage after = bar->grab().toImage();
        EXPECT_NE(before, after) << "the title bar did not follow the window title";
    }

    delete editor.data();
}
