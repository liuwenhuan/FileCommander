#pragma once

#include <atomic>

#include <QObject>
#include <QStringList>

#include "FileOpTypes.h"

// Performs the actual filesystem I/O for copy/move/delete/mkdir/rename.
// Methods here are blocking and meant to be invoked from a background
// thread (see OperationQueue); they report progress via signals, which Qt
// automatically marshals to whichever thread the receiver lives on.
class FileOperations : public QObject {
    Q_OBJECT

public:
    explicit FileOperations(QObject *parent = nullptr);

    bool copyPaths(const QStringList &sources, const QString &destDir,
                    const ConflictResolver &resolver, QString *errorMessage = nullptr);
    bool movePaths(const QStringList &sources, const QString &destDir,
                    const ConflictResolver &resolver, QString *errorMessage = nullptr);
    bool deletePaths(const QStringList &paths, bool toTrash, QString *errorMessage = nullptr);
    bool makeDirectory(const QString &parentDir, const QString &name,
                        QString *errorMessage = nullptr);
    bool renamePath(const QString &path, const QString &newName,
                     QString *errorMessage = nullptr);
    bool createSymlinks(const QStringList &sources, const QString &destDir,
                         QString *errorMessage = nullptr);

    bool wasCancelled() const { return m_cancelled.load(); }

    // Thread-safe: called from the GUI thread while a copy/move/delete runs
    // on the worker thread. The running loop polls m_cancelled between
    // entries and bails out at the next boundary (per-file granularity: an
    // in-flight single-file copy is not interrupted mid-write).
    void requestCancel() { m_cancelled.store(true); }

signals:
    // doneItems/totalItems count the top-level selected entries; doneBytes/
    // totalBytes track transferred bytes (recursive) and are 0 for operations
    // where bytes are not meaningful (delete, symlink).
    void progress(qint64 doneItems, qint64 totalItems, qint64 doneBytes, qint64 totalBytes,
                   const QString &currentFile);
    void errorOccurred(const QString &message);

private:
    bool copyOne(const QString &source, const QString &destDir, bool removeSource,
                  const ConflictResolver &resolver, ErrorAction &batchAction,
                  QString *errorMessage);
    bool copyRecursively(const QString &sourceDir, const QString &destDir);
    void emitProgress(const QString &currentFile);
    static qint64 countEntries(const QStringList &paths);
    static qint64 countBytes(const QStringList &paths);
    static QString uniqueDestination(const QString &destDir, const QString &name);

    std::atomic<bool> m_cancelled{false};
    qint64 m_totalItems = 0;
    qint64 m_doneItems = 0;
    qint64 m_totalBytes = 0;
    qint64 m_doneBytes = 0;
};
