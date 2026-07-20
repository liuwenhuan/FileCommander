#pragma once

#include <QString>
#include <QStringList>

// Splits a file into fixed-size numbered parts (name.001, name.002, ...) and
// merges such parts back into the original file. Pure blocking helpers meant
// to be called from a worker thread.
class FileSplitter {
public:
    // Splits sourcePath into parts of at most partSize bytes, written into
    // destDir as "<filename>.001", "<filename>.002", ... Returns the created
    // part paths, or an empty list on failure (message in errorMessage).
    static QStringList split(const QString &sourcePath, qint64 partSize, const QString &destDir,
                              QString *errorMessage = nullptr);

    // Merges the sequence starting at firstPartPath (which must end in a
    // ".NNN" numeric suffix) into destPath, concatenating .001, .002, ... in
    // order until a part is missing. Returns false on failure.
    static bool merge(const QString &firstPartPath, const QString &destPath,
                       QString *errorMessage = nullptr);

    // Given a ".NNN" part path, the original base name (suffix stripped), or
    // an empty string if the path has no numeric part suffix.
    static QString baseNameForPart(const QString &partPath);
};
