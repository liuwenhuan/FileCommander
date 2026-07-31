#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QIcon>
#include <QJsonDocument>
#include <QTimer>

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "AppIcon.h"
#include "FolderAssociation.h"
#include "InstanceCoordinator.h"
#include "MainWindow.h"
#include "Settings.h"
#include "Typography.h"
#include "TranslationManager.h"
#include "WindowActivation.h"
#include "diagnostics/RuntimeCounters.h"

int main(int argc, char *argv[]) {
    QElapsedTimer startupElapsed;
    startupElapsed.start();
    bool startupProbeRequested = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--startup-probe") == 0) {
            startupProbeRequested = true;
            break;
        }
    }
    // Use the native Qt xcb platform rather than deepin's dxcb: the app draws its
    // own frameless chrome (title bar, rounded corners, shadow) and relies on no
    // DTK/deepin platform behaviour, so it looks and works the same on any X11
    // desktop. Respect an explicit QT_QPA_PLATFORM override if the user set one.
#ifdef Q_OS_LINUX
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "xcb");
#endif

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
    const qint64 qApplicationConstructedMs =
        startupProbeRequested ? startupElapsed.elapsed() : -1;
    Typography::initializeSystemFont();
    const qint64 systemFontCapturedMs = startupProbeRequested ? startupElapsed.elapsed() : -1;
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
    const qint64 applicationIdentityReadyMs =
        startupProbeRequested ? startupElapsed.elapsed() : -1;

    const QStringList arguments = app.arguments();
    const int startupProbe = arguments.indexOf(QStringLiteral("--startup-probe"));
    if (startupProbe >= 0 && startupProbe + 1 >= arguments.size()) {
        qCritical("--startup-probe requires an output file path");
        return 2;
    }
    const QString startupProbeOutput = startupProbe >= 0 ? arguments.at(startupProbe + 1)
                                                         : QString();
    InstanceCoordinator instance;
    if (startupProbeOutput.isEmpty()) {
        const InstanceCoordinator::StartResult instanceResult = instance.startOrActivate(arguments);
        if (instanceResult == InstanceCoordinator::StartResult::Forwarded)
            return 0;
    }

    Settings settings;
    const qint64 settingsReadyMs = startupProbeRequested ? startupElapsed.elapsed() : -1;
    Typography::applyApplicationFont(settings);
    const qint64 applicationFontReadyMs = startupProbeRequested ? startupElapsed.elapsed() : -1;
    TranslationManager::install(app, settings.language());
    const qint64 translationReadyMs = startupProbeRequested ? startupElapsed.elapsed() : -1;
    QJsonObject startupPhases;
    if (startupProbeRequested) {
        startupPhases = {
            {QStringLiteral("qApplicationConstructedMs"), qApplicationConstructedMs},
            {QStringLiteral("systemFontCapturedMs"), systemFontCapturedMs},
            {QStringLiteral("applicationIdentityReadyMs"), applicationIdentityReadyMs},
            {QStringLiteral("settingsReadyMs"), settingsReadyMs},
            {QStringLiteral("applicationFontReadyMs"), applicationFontReadyMs},
            {QStringLiteral("translationReadyMs"), translationReadyMs},
        };
    }

    int exitCode = 0;
    {
        MainWindow window(nullptr, startupElapsed.elapsed(), !startupProbeOutput.isEmpty(),
                          &startupElapsed);
        if (!startupProbeOutput.isEmpty())
            startupPhases.insert(QStringLiteral("mainWindowConstructedMs"), startupElapsed.elapsed());
        // Belt-and-braces for WMs that read the per-window icon. Takes the
        // application icon rather than painting a fresh one: MainWindow's
        // constructor has already applied the theme, which may have replaced the
        // app icon with a recoloured variant, and painting again here would put the
        // untinted original back -- which every dialog then inherits, since
        // DialogTitleBar reads its icon from the window it belongs to.
        window.setWindowIcon(app.windowIcon());
        const QStringList folders = FolderAssociation::folderArguments(arguments);
        if (!startupProbeOutput.isEmpty()) {
            QObject::connect(&window, &MainWindow::startupReady, &app,
                             [&app, &window, startupProbeOutput, &startupPhases] {
                    QFile output(startupProbeOutput);
                    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                        std::fprintf(stderr, "FileCommander startup probe: cannot open %s: %s\n",
                                     qPrintable(startupProbeOutput), qPrintable(output.errorString()));
                        QTimer::singleShot(0, &app, [&app] { app.exit(1); });
                        return;
                    }
                    QJsonObject metrics = window.startupMetrics();
                    for (auto it = startupPhases.constBegin();
                         it != startupPhases.constEnd(); ++it) {
                        metrics.insert(it.key(), it.value());
                    }
                    const QByteArray json =
                        QJsonDocument(metrics).toJson(QJsonDocument::Compact);
                    if (output.write(json) != json.size() || !output.flush() ||
                        output.error() != QFileDevice::NoError) {
                        std::fprintf(stderr, "FileCommander startup probe: cannot write %s: %s\n",
                                     qPrintable(startupProbeOutput), qPrintable(output.errorString()));
                        output.close();
                        QTimer::singleShot(0, &app, [&app] { app.exit(1); });
                        return;
                    }
                    output.close();
                    if (output.error() != QFileDevice::NoError) {
                        std::fprintf(stderr, "FileCommander startup probe: cannot close %s: %s\n",
                                     qPrintable(startupProbeOutput), qPrintable(output.errorString()));
                        QTimer::singleShot(0, &app, [&app] { app.exit(1); });
                        return;
                    }
                    QTimer::singleShot(0, &app, [&app] { app.exit(0); });
                });
        }
        if (!startupProbeOutput.isEmpty() && !folders.isEmpty())
            window.openFolders(folders);
        if (!startupProbeOutput.isEmpty())
            startupPhases.insert(QStringLiteral("folderArgumentsProcessedMs"),
                                 startupElapsed.elapsed());
        window.show();
        if (!startupProbeOutput.isEmpty())
            startupPhases.insert(QStringLiteral("showReturnedMs"), startupElapsed.elapsed());
        ttc::requestWindowForeground(&window);
        if (startupProbeOutput.isEmpty()) {
            QObject::connect(&instance, &InstanceCoordinator::activationRequested, &window,
                             [&window](const QStringList &activationArguments) {
                    const QStringList folders = FolderAssociation::folderArguments(activationArguments);
                    if (!folders.isEmpty())
                        window.openFolders(folders);
                    ttc::requestWindowForeground(&window);
                });
        }
        const int packageSmoke = arguments.indexOf(QStringLiteral("--package-smoke"));
        if (packageSmoke >= 0 && packageSmoke + 1 < arguments.size()) {
            const QString directory = arguments.at(packageSmoke + 1);
            QTimer::singleShot(250, &window,
                               [&window, directory] { window.runPackageSmoke(directory); });
        } else if (arguments.contains(QStringLiteral("--smoke-test"))) {
            QTimer::singleShot(750, &app, [] {
                // Smoke mode only proves the packaged binary can start and draw.
                // Avoid third-party TSF/input-method DLL teardown crashes that can
                // happen after verification has already completed.
                std::_Exit(0);
            });
        } else {
            if (!folders.isEmpty())
                QTimer::singleShot(0, &window, [&window, folders] { window.openFolders(folders); });
        }

        exitCode = app.exec();
    }
    fc::reportFinalRuntimeSnapshotIfEnabled();
    return exitCode;
}
