#include "ShellShortcuts.h"

#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTextStream>

namespace {

// The first bytes of an ELF binary. An AppImage is one too -- it is an ELF
// runtime with a squashfs image appended -- so the AppImage test has to come
// first wherever the two are distinguished.
bool hasElfMagic(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray magic = file.read(4);
    return magic.size() == 4 && magic[0] == '\x7f' && magic[1] == 'E' && magic[2] == 'L' &&
           magic[3] == 'F';
}

bool looksLikeAppImage(const QString &path) {
    if (QFileInfo(path).suffix().compare(QLatin1String("appimage"), Qt::CaseInsensitive) == 0)
        return true;
    // Type-2 AppImages carry "AI\x02" at offset 8 of the ELF header. The same
    // marker packaging/build-appimage.sh checks its own output for, and the
    // only way to recognise one that has been renamed without the suffix.
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray head = file.read(11);
    return head.size() == 11 && head.startsWith(QByteArrayLiteral("\x7f" "ELF")) &&
           head[8] == 'A' && head[9] == 'I' && head[10] == '\x02';
}

// ~/Desktop as the desktop itself resolves it. xdg-user-dir knows the localised
// name (Bureau, 桌面, ...) that a hardcoded "Desktop" would miss; falling back
// to Qt's answer when it is not installed.
QString desktopDir() {
    QProcess proc;
    proc.start(QStringLiteral("xdg-user-dir"), {QStringLiteral("DESKTOP")});
    if (proc.waitForFinished(2000) && proc.exitStatus() == QProcess::NormalExit &&
        proc.exitCode() == 0) {
        const QString out = QString::fromLocal8Bit(proc.readAllStandardOutput()).trimmed();
        if (!out.isEmpty() && QFileInfo(out).isDir())
            return out;
    }
    return QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
}

QString applicationsDir() {
    // GenericDataLocation is ~/.local/share; the menu reads applications/ under
    // it. Created on demand -- a system that has never installed a user-level
    // launcher will not have it yet.
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (base.isEmpty())
        return {};
    return QDir(base).filePath(QStringLiteral("applications"));
}

// Escapes a value for a .desktop key. Only backslash and newline can break the
// file's line-oriented format; quotes and spaces are literal in a plain key.
QString desktopEscape(const QString &value) {
    QString out = value;
    out.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
    out.replace(QLatin1Char('\n'), QLatin1String("\\n"));
    return out;
}

} // namespace

namespace fc::ShellShortcuts {

bool isSupported() {
    return true;
}

bool supports(Destination where) {
    // No Startup here. Autostart is a real thing on Linux
    // (~/.config/autostart), but it is a separate promise from "put this in my
    // menu" and is not what was asked for; offering it half-built would be
    // worse than not offering it.
    return where == Destination::Desktop || where == Destination::Applications;
}

QString locationFor(Destination where) {
    switch (where) {
    case Destination::Desktop:
        return desktopDir();
    case Destination::Applications:
        return applicationsDir();
    case Destination::Startup:
        return {};
    }
    return {};
}

bool isLaunchable(const QString &path) {
    if (path.isEmpty())
        return false;
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile())
        return false;
    // By content, not by name: a Linux executable usually has no suffix, and an
    // AppImage that has lost its .AppImage suffix is still one.
    return hasElfMagic(path);
}

bool needsExecutableBit(const QString &path) {
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile() || info.isExecutable())
        return false;
    return looksLikeAppImage(path);
}

PlatformResult makeExecutable(const QString &path) {
    QFile file(path);
    if (!file.exists())
        return PlatformResult::failure(PlatformError::NotFound,
                                       QStringLiteral("No such file: %1").arg(path));
    const QFileDevice::Permissions current = file.permissions();
    // Only the execute bits, and only where a read bit already allows access:
    // granting group/other execute on a file they cannot read would be an odd
    // thing for a file manager to do behind the user's back.
    QFileDevice::Permissions wanted = current | QFileDevice::ExeOwner;
    if (current & QFileDevice::ReadGroup)
        wanted |= QFileDevice::ExeGroup;
    if (current & QFileDevice::ReadOther)
        wanted |= QFileDevice::ExeOther;
    if (!file.setPermissions(wanted))
        return PlatformResult::failure(PlatformError::PermissionDenied,
                                       QStringLiteral("Could not change the permissions of %1")
                                           .arg(path));
    return PlatformResult::success();
}

PlatformResult create(const QString &targetPath, Destination where) {
    if (!supports(where))
        return PlatformResult::failure(PlatformError::Unsupported,
                                       QStringLiteral("Not available on this platform."));

    const QFileInfo target(targetPath);
    if (!target.exists() || !target.isFile())
        return PlatformResult::failure(PlatformError::NotFound,
                                       QStringLiteral("No such file: %1").arg(targetPath));

    const QString folder = locationFor(where);
    if (folder.isEmpty())
        return PlatformResult::failure(PlatformError::NativeFailure,
                                       QStringLiteral("Could not locate the destination folder."));
    if (!QDir().mkpath(folder))
        return PlatformResult::failure(PlatformError::PermissionDenied,
                                       QStringLiteral("Could not create %1").arg(folder));

    const QString name = target.completeBaseName();
    const QString file = QDir(folder).filePath(name + QLatin1String(".desktop"));

    QFile out(file);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return PlatformResult::failure(PlatformError::PermissionDenied,
                                       QStringLiteral("Could not write %1").arg(file));
    {
        QTextStream ts(&out);
        ts.setCodec("UTF-8");
        // Exec is quoted because a path may contain spaces, and Path is set for
        // the same reason the Windows shortcut sets a working directory: a
        // program started from a launcher without one inherits the session's.
        ts << "[Desktop Entry]\n"
           << "Type=Application\n"
           << "Version=1.0\n"
           << "Name=" << desktopEscape(name) << "\n"
           << "Exec=\"" << desktopEscape(target.absoluteFilePath()) << "\"\n"
           << "Path=" << desktopEscape(target.absolutePath()) << "\n"
           << "Icon=" << desktopEscape(target.absoluteFilePath()) << "\n"
           << "Terminal=false\n"
           << "Categories=Utility;\n";
    }
    out.close();

    // A .desktop file on the DESKTOP must be executable or GNOME and KDE show
    // it as a text file rather than a launcher. (In the applications menu the
    // bit is not required, but setting it there too costs nothing and keeps the
    // two identical.) This is the step that makes the difference between a
    // working icon and one that opens a text editor.
    QFile perms(file);
    perms.setPermissions(perms.permissions() | QFileDevice::ExeOwner);

    // GNOME additionally requires the file to be marked trusted before it will
    // run one placed on the desktop. gio is part of the same glib stack GNOME
    // itself ships, and is a no-op elsewhere; failure is ignored because the
    // launcher is still valid in every other environment.
    if (where == Destination::Desktop) {
        QProcess::execute(QStringLiteral("gio"),
                          {QStringLiteral("set"), file, QStringLiteral("metadata::trusted"),
                           QStringLiteral("true")});
    }
    return PlatformResult::success();
}

} // namespace fc::ShellShortcuts
