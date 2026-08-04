#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace fc {

// One application the system knows how to open a file with.
//
// `program`/`arguments` are what to run; `token` is an opaque platform handle
// (a Windows ProgID or an XDG desktop-file id) which the launcher prefers when
// it is set, because it can open files a bare command line cannot -- a Windows
// Store app has no executable path at all, and a desktop file may carry field
// codes and a working directory that matter.
struct OpenWithHandler {
    QString displayName;
    QString program;
    QStringList arguments;
    QString iconPath; // file to take the icon from; may be the program itself
    int iconIndex = 0;
    QString token;
    bool recommended = false; // registered for this file's type
};

// Everything that can open `filePath`, recommended first and then the rest,
// each group alphabetical. Empty when the platform has nothing to say -- the
// caller still offers "choose an application yourself", which is the one
// option that never depends on the system's registrations.
QVector<OpenWithHandler> openWithHandlers(const QString &filePath);

// Runs one of them on a file. Returns false if the application could not be
// started at all; a program that starts and then complains about the file is a
// success as far as this is concerned.
bool launchOpenWithHandler(const OpenWithHandler &handler, const QString &filePath);

// Drops duplicates and orders the list the way a menu wants it: recommended
// handlers first, each group by display name, case-insensitively.
//
// Deduplication is by what would actually be launched (token, else the program
// path), not by display name: two registrations of the same application under
// slightly different names are the same menu entry, while two applications
// that happen to share a name are not. A handler that is recommended in one
// registration and not in another counts as recommended.
QVector<OpenWithHandler> tidyOpenWithHandlers(QVector<OpenWithHandler> handlers);

} // namespace fc
