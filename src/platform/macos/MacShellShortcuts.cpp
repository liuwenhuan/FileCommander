#include "ShellShortcuts.h"

#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

namespace {

const QString kDesktopScript = QStringLiteral(
    "on run argv\n"
    "set targetPath to item 1 of argv\n"
    "set aliasName to item 2 of argv\n"
    "tell application \"Finder\"\n"
    "set desktopFolder to desktop\n"
    "try\n"
    "delete (some item of desktopFolder whose name is aliasName)\n"
    "end try\n"
    "set newAlias to make new alias file at desktopFolder to (POSIX file targetPath as alias)\n"
    "set name of newAlias to aliasName\n"
    "end tell\n"
    "end run");

PlatformResult runAppleScript(const QStringList &arguments) {
    QProcess process;
    process.start(QStringLiteral("/usr/bin/osascript"),
                  QStringList{QStringLiteral("-e"), kDesktopScript} + arguments);
    if (!process.waitForStarted())
        return PlatformResult::failure(
            PlatformError::NativeFailure,
            QStringLiteral("Could not start Finder automation."));
    if (!process.waitForFinished(-1))
        return PlatformResult::failure(
            PlatformError::NativeFailure,
            QStringLiteral("Finder automation did not finish."));
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString error =
            QString::fromUtf8(process.readAllStandardError()).trimmed();
        return PlatformResult::failure(
            PlatformError::NativeFailure,
            error.isEmpty() ? QStringLiteral("Finder could not create the alias.") : error,
            process.exitCode());
    }
    return PlatformResult::success();
}

} // namespace

namespace fc::ShellShortcuts {

bool isSupported() {
    return true;
}

bool supports(Destination where) {
    return where == Destination::Desktop;
}

QString locationFor(Destination where) {
    if (where != Destination::Desktop)
        return {};
    return QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
}

bool isLaunchable(const QString &path) {
    const QFileInfo info(path);
    if (!info.exists())
        return false;
    if (info.isDir())
        return info.fileName().endsWith(QStringLiteral(".app"), Qt::CaseInsensitive);
    return info.isExecutable();
}

bool needsExecutableBit(const QString &) {
    return false;
}

PlatformResult makeExecutable(const QString &) {
    return PlatformResult::failure(
        PlatformError::Unsupported,
        QStringLiteral("macOS launchable items do not require an AppImage executable-bit fix."));
}

PlatformResult create(const QString &targetPath, Destination where) {
    if (!supports(where))
        return PlatformResult::failure(
            PlatformError::Unsupported,
            QStringLiteral("This shortcut destination is not available."));

    const QFileInfo target(targetPath);
    if (!target.exists() && !target.isSymLink())
        return PlatformResult::failure(PlatformError::NotFound,
                                       QStringLiteral("The target does not exist."));

    const QString desktop = locationFor(where);
    if (desktop.isEmpty() || !QFileInfo(desktop).isDir())
        return PlatformResult::failure(
            PlatformError::NativeFailure,
            QStringLiteral("The Desktop folder is not available."));

    QString aliasName = target.fileName();
    if (aliasName.isEmpty())
        aliasName = target.absoluteFilePath();
    return runAppleScript({target.absoluteFilePath(), aliasName});
}

} // namespace fc::ShellShortcuts
