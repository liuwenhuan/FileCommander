#include <QApplication>
#include <QColor>
#include <QIcon>
#include <QPainter>
#include <QPixmap>

#include <clocale>

#include "MainWindow.h"
#include "Settings.h"
#include "TranslationManager.h"

namespace {

// Paint the app icon at a given size. Doing it in code (rather than a theme
// lookup or an SVG resource) guarantees a visible title-bar/taskbar icon on
// every desktop, independent of the icon theme or Qt SVG plugin.
QPixmap paintIcon(int size) {
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal s = size / 64.0;
    p.scale(s, s);
    p.setPen(Qt::NoPen);

    // Rounded body + darker title strip.
    p.setBrush(QColor(0x2c, 0x7b, 0xe5));
    p.drawRoundedRect(4, 8, 56, 48, 8, 8);
    p.setBrush(QColor(0x1b, 0x5f, 0xc0));
    p.drawRoundedRect(4, 8, 56, 16, 8, 8);
    p.drawRect(4, 16, 56, 8); // square off the strip's lower edge

    // Two panes with a few "file" rows, echoing the dual-pane layout.
    const QColor rowColor(0x9c, 0xc0, 0xf0);
    for (int pane = 0; pane < 2; ++pane) {
        const int x = 9 + pane * 26;
        p.setBrush(pane == 0 ? Qt::white : QColor(0xdb, 0xe7, 0xfb));
        p.drawRoundedRect(x, 28, 20, 24, 3, 3);
        p.setBrush(rowColor);
        for (int row = 0; row < 3; ++row)
            p.drawRect(x + 4, 33 + row * 5, 12, 2);
    }
    p.end();
    return pm;
}

QIcon appIcon() {
    QIcon icon;
    for (int size : {16, 24, 32, 48, 64})
        icon.addPixmap(paintIcon(size));
    return icon;
}

} // namespace

int main(int argc, char *argv[]) {
    // Use the native Qt xcb platform rather than deepin's dxcb: the app draws its
    // own frameless chrome (title bar, rounded corners, shadow) and relies on no
    // DTK/deepin platform behaviour, so it looks and works the same on any X11
    // desktop. Respect an explicit QT_QPA_PLATFORM override if the user set one.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "xcb");

    QApplication app(argc, argv);
    // libmpv (video preview) refuses to create a context unless LC_NUMERIC is
    // "C". Qt/system locale otherwise sets it to the user's locale, which makes
    // mpv_create() throw. Reset just the numeric category (keeps the rest of the
    // locale for translated text) right after QApplication has done its setup.
    std::setlocale(LC_NUMERIC, "C");
    app.setApplicationName("ttc");
    app.setOrganizationName("ttc");
    // deepin's dxcb title bar resolves its icon from the matching .desktop
    // file rather than _NET_WM_ICON, so point it at ours (installed as
    // ttc.desktop with Icon=ttc). Harmless on other desktops.
    app.setDesktopFileName(QStringLiteral("ttc"));
    app.setWindowIcon(appIcon());

    Settings settings;
    TranslationManager::install(app, settings.language());

    MainWindow window;
    window.setWindowIcon(appIcon()); // belt-and-suspenders for WMs that read the per-window icon
    window.show();

    return app.exec();
}
