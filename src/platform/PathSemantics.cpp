#include "PathSemantics.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

namespace {
QString windowsClean(QString path) {
    path.replace(QLatin1Char('/'), QLatin1Char('\\'));
    if (path.size() > 3)
        while (path.endsWith(QLatin1Char('\\')))
            path.chop(1);
    return path;
}
}

bool PathSemantics::isRoot(const QString &path, PathFlavor flavor) {
    if (flavor == PathFlavor::Posix)
        return QDir::cleanPath(path) == QLatin1String("/");
    const QString p = windowsClean(path);
    static const QRegularExpression drive(QStringLiteral("^[A-Za-z]:\\\\$"));
    static const QRegularExpression unc(QStringLiteral("^\\\\\\\\[^\\\\]+\\\\[^\\\\]+$"));
    return drive.match(p).hasMatch() || unc.match(p).hasMatch();
}

QString PathSemantics::parentPath(const QString &path, PathFlavor flavor) {
    if (isRoot(path, flavor))
        return flavor == PathFlavor::Windows ? windowsClean(path) : QStringLiteral("/");
    if (flavor == PathFlavor::Posix)
        return QFileInfo(QDir::cleanPath(path)).dir().path();
    QString p = windowsClean(path);
    const int slash = p.lastIndexOf(QLatin1Char('\\'));
    if (slash < 0)
        return {};
    const QString parent = p.left(slash);
    return parent.size() == 2 && parent.at(1) == QLatin1Char(':')
               ? parent + QLatin1Char('\\')
               : parent;
}

bool PathSemantics::equivalent(const QString &left, const QString &right, PathFlavor flavor) {
    if (flavor == PathFlavor::Windows)
        return windowsClean(left).compare(windowsClean(right), Qt::CaseInsensitive) == 0;
    return QDir::cleanPath(left) == QDir::cleanPath(right);
}

bool PathSemantics::isInsideOrSame(const QString &path, const QString &ancestor,
                                   PathFlavor flavor) {
    if (path.isEmpty() || ancestor.isEmpty())
        return false;
    if (equivalent(path, ancestor, flavor))
        return true;

    const bool windows = flavor == PathFlavor::Windows;
    const QChar separator = windows ? QLatin1Char('\\') : QLatin1Char('/');
    QString child = windows ? windowsClean(path) : QDir::cleanPath(path);
    QString parent = windows ? windowsClean(ancestor) : QDir::cleanPath(ancestor);
    // A root keeps its trailing separator ("C:" + backslash, "/"), and appending
    // another would leave a doubled one that never matches.
    if (!parent.endsWith(separator))
        parent.append(separator);
    if (child.size() <= parent.size())
        return false;
    return windows ? child.startsWith(parent, Qt::CaseInsensitive) : child.startsWith(parent);
}

PlatformResult PathSemantics::validateComponent(const QString &component, PathFlavor flavor) {
    if (component.isEmpty() || component == QLatin1String(".") ||
        component == QLatin1String(".."))
        return PlatformResult::failure(PlatformError::InvalidPath,
                                       QStringLiteral("The file name is not valid."));
    if (flavor == PathFlavor::Posix)
        return component.contains(QLatin1Char('/'))
                   ? PlatformResult::failure(PlatformError::InvalidPath,
                                             QStringLiteral("A file name cannot contain '/'."))
                   : PlatformResult::success();
    if (component.endsWith(QLatin1Char('.')) || component.endsWith(QLatin1Char(' ')) ||
        component.contains(QRegularExpression(QStringLiteral(R"([<>:"/\\|?*\x00-\x1f])"))))
        return PlatformResult::failure(PlatformError::InvalidPath,
                                       QStringLiteral("The file name contains characters Windows does not allow."));
    const QString stem = component.section(QLatin1Char('.'), 0, 0).toUpper();
    static const QSet<QString> reserved = {
        QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"),
        QStringLiteral("NUL"), QStringLiteral("COM1"), QStringLiteral("COM2"),
        QStringLiteral("COM3"), QStringLiteral("COM4"), QStringLiteral("COM5"),
        QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
        QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"),
        QStringLiteral("LPT3"), QStringLiteral("LPT4"), QStringLiteral("LPT5"),
        QStringLiteral("LPT6"), QStringLiteral("LPT7"), QStringLiteral("LPT8"),
        QStringLiteral("LPT9")};
    return reserved.contains(stem)
               ? PlatformResult::failure(PlatformError::InvalidPath,
                                         QStringLiteral("The file name is reserved by Windows."))
               : PlatformResult::success();
}

bool PathSemantics::requiresCaseOnlyRename(const QString &from, const QString &to,
                                           PathFlavor flavor) {
    return flavor == PathFlavor::Windows && from != to &&
           equivalent(from, to, PathFlavor::Windows);
}
