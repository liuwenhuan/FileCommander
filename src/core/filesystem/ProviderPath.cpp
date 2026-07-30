#include "ProviderPath.h"

#include <QStringList>

namespace fc::ProviderPath {

QString normalizeRooted(const QString &path) {
    QStringList components;
    for (const QString &component :
         path.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (component == QStringLiteral("."))
            continue;
        if (component == QStringLiteral("..")) {
            if (!components.isEmpty())
                components.removeLast();
            continue;
        }
        components.append(component);
    }

    return components.isEmpty()
               ? QStringLiteral("/")
               : QLatin1Char('/') + components.join(QLatin1Char('/'));
}

QString parent(const QString &path) {
    const QString normalized = normalizeRooted(path);
    if (normalized == QStringLiteral("/"))
        return {};

    const int slash = normalized.lastIndexOf(QLatin1Char('/'));
    return slash <= 0 ? QStringLiteral("/") : normalized.left(slash);
}

QString sibling(const QString &path, const QString &newName) {
    if (newName.isEmpty() || newName == QStringLiteral(".") ||
        newName == QStringLiteral("..") ||
        newName.contains(QLatin1Char('/')) ||
        newName.contains(QLatin1Char('\\')))
        return {};

    const QString normalized = normalizeRooted(path);
    if (normalized == QStringLiteral("/"))
        return {};

    const QString parentPath = parent(normalized);
    return parentPath == QStringLiteral("/")
               ? parentPath + newName
               : parentPath + QLatin1Char('/') + newName;
}

} // namespace fc::ProviderPath
