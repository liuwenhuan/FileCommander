#include <QApplication>
#include <QGuiApplication>
#include <QIcon>

#include <clocale>

#include "AppIcon.h"
#include "MainWindow.h"
#include "Settings.h"
#include "TranslationManager.h"

int main(int argc, char *argv[]) {
    // Use the native Qt xcb platform rather than deepin's dxcb: the app draws its
    // own frameless chrome (title bar, rounded corners, shadow) and relies on no
    // DTK/deepin platform behaviour, so it looks and works the same on any X11
    // desktop. Respect an explicit QT_QPA_PLATFORM override if the user set one.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "xcb");

    // High-DPI support. Qt 5 leaves per-screen scaling off by default, so on a
    // HiDPI display the window would render at 1x and the compositor would
    // upscale it (blurry + extra work). Enable device-pixel scaling and @2x
    // pixmaps. Honour the desktop's fractional factor verbatim (deepin commonly
    // uses 1.25/1.5) instead of rounding to an integer, so FileCommander matches
    // the size of native apps. AA_ShareOpenGLContexts lets the mpv QOpenGLWidget keep its
    // GL resources across reparenting (the Ctrl+Q preview swap). All of these
    // must be set before the QApplication is constructed.
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
#endif
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    QApplication app(argc, argv);
    // libmpv (video preview) refuses to create a context unless LC_NUMERIC is
    // "C". Qt/system locale otherwise sets it to the user's locale, which makes
    // mpv_create() throw. Reset just the numeric category (keeps the rest of the
    // locale for translated text) right after QApplication has done its setup.
    std::setlocale(LC_NUMERIC, "C");
    app.setApplicationName("FileCommander");
    app.setOrganizationName("FileCommander");
    // deepin's dxcb title bar resolves its icon from the matching .desktop
    // file rather than _NET_WM_ICON, so point it at ours (installed as
    // FileCommander.desktop with Icon=FileCommander). Harmless on other desktops.
    app.setDesktopFileName(QStringLiteral("FileCommander"));
    app.setWindowIcon(ttc::appIcon());

    Settings settings;
    TranslationManager::install(app, settings.language());

    MainWindow window;
    // Belt-and-braces for WMs that read the per-window icon. Takes the
    // application icon rather than painting a fresh one: MainWindow's
    // constructor has already applied the theme, which may have replaced the
    // app icon with a recoloured variant, and painting again here would put the
    // untinted original back -- which every dialog then inherits, since
    // DialogTitleBar reads its icon from the window it belongs to.
    window.setWindowIcon(app.windowIcon());
    window.show();

    return app.exec();
}
