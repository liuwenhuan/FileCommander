#pragma once

#include <QStringList>

class FolderAssociation {
public:
    // Extracts existing local directories from argv-style input. File paths and
    // application options are deliberately ignored: this integration only owns
    // folder and drive activation.
    static QStringList folderArguments(const QStringList &arguments);

    // Registers or removes FileCommander's per-user folder/drive open action.
    // `stateFilePath` stores the previous user choice so disabling restores it.
    static bool setEnabled(bool enabled, const QString &stateFilePath, QString *error);

#ifdef Q_OS_WIN
    static QString windowsOpenVerb();
#endif
};
