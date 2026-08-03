#include "UpdateSmoke.h"

#include <QCoreApplication>
#include <QFile>
#include <QStringList>
#include <QTextStream>
#include <QTimer>

#include "update/UpdateChecker.h"
#include "update/Updater.h"

#include "version.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <cstdio>
#endif

namespace {

QString g_logPath;

void say(const QString &line) {
    QTextStream out(stdout);
    out << line << Qt::endl;
    out.flush();
    if (g_logPath.isEmpty())
        return;
    QFile log(g_logPath);
    if (log.open(QIODevice::WriteOnly | QIODevice::Append))
        log.write(line.toUtf8() + '\n');
}

// The Windows build is linked as a GUI subsystem binary, so it starts with no
// stdout at all and everything say() writes would go nowhere. Borrow the
// console of whatever launched us, which is what makes this usable from a shell.
void attachToParentConsole() {
#ifdef Q_OS_WIN
    if (!AttachConsole(ATTACH_PARENT_PROCESS))
        return;
    FILE *stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
#endif
}

} // namespace

namespace fc {

int runUpdateSmoke(QCoreApplication &app) {
    attachToParentConsole();
    // An optional path after the flag also receives every line, because the
    // interesting run is the one that ends with this process exiting so its
    // replacement can take over -- and a console that closes with it.
    const QStringList arguments = app.arguments();
    const int flag = arguments.indexOf(QStringLiteral("--update-smoke"));
    if (flag >= 0 && flag + 1 < arguments.size()
        && !arguments.at(flag + 1).startsWith(QLatin1String("--"))) {
        g_logPath = arguments.at(flag + 1);
        QFile::remove(g_logPath);
    }

    say(QStringLiteral("update-smoke: running version %1").arg(QStringLiteral(TTC_VERSION)));
    say(QStringLiteral("update-smoke: manifest  %1").arg(UpdateChecker::manifestUrl()));
    say(QStringLiteral("update-smoke: segment   %1").arg(UpdateChecker::packageSegmentKey()));

    int exitCode = 1;
    auto *checker = new UpdateChecker(&app);
    auto *updater = new Updater(&app);

    QObject::connect(checker, &UpdateChecker::noUpdate, &app, [&exitCode, &app] {
        say(QStringLiteral("update-smoke: already up to date"));
        exitCode = 2;
        app.quit();
    });
    QObject::connect(checker, &UpdateChecker::checkFailed, &app,
                     [&exitCode, &app](const QString &error) {
                         say(QStringLiteral("update-smoke: CHECK FAILED: %1").arg(error));
                         exitCode = 1;
                         app.quit();
                     });
    QObject::connect(checker, &UpdateChecker::updateAvailable, &app,
                     [updater](const UpdateInfo &info) {
                         say(QStringLiteral("update-smoke: found %1 (%2)")
                                 .arg(info.version, info.date));
                         say(QStringLiteral("update-smoke: package  %1").arg(info.url));
                         say(QStringLiteral("update-smoke: sha256   %1").arg(info.sha256));
                         updater->apply(info);
                     });

    // One line per decile rather than per packet: this is read from a log.
    auto *lastDecile = new int(-1);
    QObject::connect(updater, &Updater::progress, &app, [lastDecile](int percent) {
        if (percent / 10 == *lastDecile)
            return;
        *lastDecile = percent / 10;
        say(QStringLiteral("update-smoke: downloading %1%").arg(percent));
    });
    QObject::connect(updater, &Updater::finished, &app,
                     [&exitCode, &app](bool ok, const QString &message) {
                         say(QStringLiteral("update-smoke: %1: %2")
                                 .arg(ok ? QStringLiteral("INSTALLED") : QStringLiteral("FAILED"),
                                      message));
                         exitCode = ok ? 0 : 1;
                         // On success the installer is already waiting for this
                         // process to exit before it can replace the files, so
                         // leaving promptly is part of the flow being tested.
                         app.quit();
                     });

    QTimer::singleShot(0, checker, [checker] { checker->checkForUpdates(); });
    app.exec();
    return exitCode;
}

} // namespace fc
