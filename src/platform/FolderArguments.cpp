#include "FolderArguments.h"

#include <QDir>
#include <QFileInfo>

QStringList FolderArguments::folders(const QStringList &arguments) {
    QStringList folders;
    for (int i = 1; i < arguments.size(); ++i) {
        const QString argument = arguments.at(i);
        if (argument.startsWith(QLatin1Char('-')))
            continue;
        const QFileInfo info(argument);
        if (!info.isDir())
            continue;
        const QString path = QDir::cleanPath(info.absoluteFilePath());
        if (!folders.contains(path))
            folders.append(path);
    }
    return folders;
}
