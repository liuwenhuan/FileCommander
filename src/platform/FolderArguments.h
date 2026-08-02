#pragma once

#include <QStringList>

// Command-line folder handling. This is only about reading argv: it stays
// useful however the application was launched (a shell "open with", a shortcut,
// a second instance handing its arguments to the first), and is deliberately
// separate from registering the application as the system's folder handler --
// that integration was removed.
class FolderArguments {
public:
    // Extracts existing local directories from argv-style input. File paths and
    // application options are deliberately ignored: this only owns folder and
    // drive activation.
    static QStringList folders(const QStringList &arguments);
};
