#include "OpenWithHandlers.h"

#include <algorithm>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QMimeType>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QTextStream>

namespace fc {

namespace {

// Where .desktop files live, in XDG precedence order.
QStringList applicationDirectories() {
    QStringList dirs;
    for (const QString &base : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation)) {
        const QString candidate = base + QStringLiteral("/applications");
        if (QFileInfo::exists(candidate))
            dirs.append(candidate);
    }
    return dirs;
}

struct DesktopEntry {
    QString id;
    QString name;
    QString icon;
    QString exec;
    QStringList mimeTypes;
    bool hidden = false;
    bool terminal = false;
};

// A .desktop file is an ini file, but QSettings mangles keys with locale
// suffixes ("Name[zh_CN]") and Exec's quoting, so it is read directly. Only the
// [Desktop Entry] group matters here.
DesktopEntry readDesktopFile(const QString &path, const QString &id) {
    DesktopEntry entry;
    entry.id = id;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return entry;
    QTextStream stream(&file);
    bool inMainGroup = false;
    while (!stream.atEnd()) {
        const QString line = stream.readLine().trimmed();
        if (line.startsWith(QLatin1Char('['))) {
            inMainGroup = line == QStringLiteral("[Desktop Entry]");
            continue;
        }
        if (!inMainGroup || line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        const int equals = line.indexOf(QLatin1Char('='));
        if (equals <= 0)
            continue;
        const QString key = line.left(equals).trimmed();
        const QString value = line.mid(equals + 1).trimmed();
        // The unsuffixed key is the untranslated name; a localised one is
        // preferred only when it matches this session's language.
        if (key == QStringLiteral("Name") && entry.name.isEmpty()) {
            entry.name = value;
        } else if (key.startsWith(QStringLiteral("Name[")) &&
                   key.contains(QLocale::system().name())) {
            entry.name = value;
        } else if (key == QStringLiteral("Icon")) {
            entry.icon = value;
        } else if (key == QStringLiteral("Exec")) {
            entry.exec = value;
        } else if (key == QStringLiteral("MimeType")) {
            entry.mimeTypes = value.split(QLatin1Char(';'), Qt::SkipEmptyParts);
        } else if (key == QStringLiteral("NoDisplay") || key == QStringLiteral("Hidden")) {
            entry.hidden = entry.hidden || value.compare(QStringLiteral("true"),
                                                         Qt::CaseInsensitive) == 0;
        } else if (key == QStringLiteral("Terminal")) {
            entry.terminal = value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
        } else if (key == QStringLiteral("Type") && value != QStringLiteral("Application")) {
            entry.hidden = true; // links and directories are not launchers
        }
    }
    return entry;
}

// Exec carries field codes: %f/%F/%u/%U stand for the file, and %i/%c/%k for
// things a menu does not need. The file is appended by the launcher, so every
// code is simply dropped here.
QStringList execArguments(const QString &exec, QString *program) {
    QStringList parts = QProcess::splitCommand(exec);
    // erase-remove, not QList::removeIf: that one is Qt 6.1 and this project
    // builds against Qt 5.15. It compiles on Windows because this file is only
    // built on Linux, so nothing caught it there.
    parts.erase(std::remove_if(parts.begin(), parts.end(),
                               [](const QString &part) {
                                   return part.size() == 2 &&
                                          part.startsWith(QLatin1Char('%'));
                               }),
                parts.end());
    if (parts.isEmpty())
        return {};
    *program = parts.takeFirst();
    return parts;
}

} // namespace

QVector<OpenWithHandler> openWithHandlers(const QString &filePath) {
    QMimeDatabase mimeDatabase;
    const QMimeType mime = mimeDatabase.mimeTypeForFile(filePath);
    QSet<QString> wanted;
    if (mime.isValid()) {
        wanted.insert(mime.name());
        for (const QString &parent : mime.allAncestors())
            wanted.insert(parent);
    }

    QVector<OpenWithHandler> found;
    QSet<QString> seenIds;
    for (const QString &directory : applicationDirectories()) {
        QDir dir(directory);
        for (const QString &name : dir.entryList({QStringLiteral("*.desktop")}, QDir::Files)) {
            // The first directory to carry an id wins, which is what XDG
            // precedence means: a user's copy shadows the system's.
            if (seenIds.contains(name))
                continue;
            seenIds.insert(name);
            const DesktopEntry entry = readDesktopFile(dir.filePath(name), name);
            if (entry.hidden || entry.exec.isEmpty() || entry.terminal)
                continue;
            OpenWithHandler handler;
            handler.displayName = entry.name.isEmpty() ? QFileInfo(name).completeBaseName()
                                                       : entry.name;
            handler.arguments = execArguments(entry.exec, &handler.program);
            if (handler.program.isEmpty())
                continue;
            handler.iconPath = entry.icon;
            handler.token = entry.id;
            for (const QString &type : entry.mimeTypes) {
                if (wanted.contains(type.trimmed())) {
                    handler.recommended = true;
                    break;
                }
            }
            found.append(handler);
        }
    }
    return tidyOpenWithHandlers(std::move(found));
}

bool launchOpenWithHandler(const OpenWithHandler &handler, const QString &filePath) {
    if (handler.program.isEmpty())
        return false;
    QStringList arguments = handler.arguments;
    arguments.append(filePath);
    return QProcess::startDetached(handler.program, arguments);
}

} // namespace fc
