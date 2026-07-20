#include "FileOperations.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>

FileOperations::FileOperations(QObject *parent) : QObject(parent) {}

void FileOperations::requestCancel() {
    m_cancelled.store(true);
    // Wake a paused worker so it can observe the cancellation and unwind.
    QMutexLocker lock(&m_pauseMutex);
    m_paused = false;
    m_pauseCond.wakeAll();
}

void FileOperations::requestPause() {
    QMutexLocker lock(&m_pauseMutex);
    m_paused = true;
}

void FileOperations::requestResume() {
    QMutexLocker lock(&m_pauseMutex);
    m_paused = false;
    m_pauseCond.wakeAll();
}

void FileOperations::waitIfPaused() {
    QMutexLocker lock(&m_pauseMutex);
    while (m_paused && !m_cancelled.load())
        m_pauseCond.wait(&m_pauseMutex);
}

qint64 FileOperations::countEntries(const QStringList &paths) {
    qint64 count = 0;
    for (const QString &path : paths) {
        QFileInfo info(path);
        if (info.isDir()) {
            count += 1;
            QDirIterator it(path, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden,
                             QDirIterator::Subdirectories);
            while (it.hasNext()) {
                it.next();
                ++count;
            }
        } else {
            count += 1;
        }
    }
    return count;
}

qint64 FileOperations::countBytes(const QStringList &paths) {
    qint64 total = 0;
    for (const QString &path : paths) {
        QFileInfo info(path);
        if (info.isDir()) {
            QDirIterator it(path, QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden,
                             QDirIterator::Subdirectories);
            while (it.hasNext()) {
                it.next();
                total += it.fileInfo().size();
            }
        } else {
            total += info.size();
        }
    }
    return total;
}

void FileOperations::emitProgress(const QString &currentFile) {
    emit progress(m_doneItems, m_totalItems, m_doneBytes, m_totalBytes, currentFile);
}

ErrorAction FileOperations::resolveError(const QString &path, const QString &error) {
    if (m_errorBatch == ErrorAction::SkipAll)
        return ErrorAction::SkipAll; // "skip all" already chosen for this batch
    const ErrorAction action = m_errorResolver ? m_errorResolver(path, error) : ErrorAction::Skip;
    if (action == ErrorAction::SkipAll)
        m_errorBatch = ErrorAction::SkipAll;
    if (action == ErrorAction::Cancel)
        m_cancelled = true;
    return action;
}

QString FileOperations::uniqueDestination(const QString &destDir, const QString &name) {
    QFileInfo fi(name);
    const QString base = fi.completeBaseName();
    const QString suffix = fi.suffix();
    int n = 1;
    QString candidate = name;
    while (QFileInfo::exists(QDir(destDir).filePath(candidate))) {
        candidate = suffix.isEmpty() ? QStringLiteral("%1 (%2)").arg(base).arg(n)
                                      : QStringLiteral("%1 (%2).%3").arg(base).arg(n).arg(suffix);
        ++n;
    }
    return candidate;
}

bool FileOperations::copyRecursively(const QString &sourceDir, const QString &destDir) {
    QDir().mkpath(destDir);
    QDirIterator it(sourceDir, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
    while (it.hasNext()) {
        it.next();
        const QFileInfo entry = it.fileInfo();
        const QString target = QDir(destDir).filePath(entry.fileName());
        if (entry.isDir()) {
            if (!copyRecursively(entry.filePath(), target))
                return false;
        } else {
            QFile::remove(target);
            if (!QFile::copy(entry.filePath(), target))
                return false;
            m_doneBytes += entry.size();
        }
        emitProgress(entry.filePath());
        if (m_cancelled)
            return false;
    }
    return true;
}

bool FileOperations::copyOne(const QString &source, const QString &destDir, bool removeSource,
                              const ConflictResolver &resolver, ErrorAction &batchAction,
                              QString *errorMessage) {
    QFileInfo srcInfo(source);
    QString destName = srcInfo.fileName();
    QString destPath = QDir(destDir).filePath(destName);

    // Dropping/pasting an entry into the directory it already lives in.
    // We must never run the remove+copy path below on this: destPath IS the
    // source, so removing it would destroy the very file we're copying.
    if (QDir::cleanPath(destPath) == QDir::cleanPath(source)) {
        if (removeSource)
            return true; // move onto itself: nothing to do
        // Copy onto itself: produce "name (1).ext" and leave the original
        // untouched, mirroring what Total Commander / file managers do.
        destPath = QDir(destDir).filePath(uniqueDestination(destDir, destName));
    } else if (QFileInfo::exists(destPath)) {
        ErrorAction action = batchAction;
        if (action != ErrorAction::OverwriteAll && action != ErrorAction::SkipAll)
            action = resolver ? resolver(source, destPath) : ErrorAction::Skip;

        if (action == ErrorAction::OverwriteAll || action == ErrorAction::SkipAll)
            batchAction = action;

        if (action == ErrorAction::Cancel) {
            m_cancelled = true;
            return false;
        }
        if (action == ErrorAction::Skip || action == ErrorAction::SkipAll)
            return true;
        if (action == ErrorAction::Rename)
            destPath = QDir(destDir).filePath(uniqueDestination(destDir, destName));
        // Overwrite / OverwriteAll: fall through and replace destPath as-is.
    }

    while (true) {
        bool ok;
        if (srcInfo.isDir()) {
            ok = copyRecursively(source, destPath);
        } else {
            QFile::remove(destPath);
            ok = QFile::copy(source, destPath);
            if (ok)
                m_doneBytes += srcInfo.size();
        }
        if (ok)
            break;

        const QString msg = tr("Failed to copy %1 to %2").arg(source, destPath);
        if (resolveError(source, msg) == ErrorAction::Retry)
            continue; // user asked to retry
        if (errorMessage)
            *errorMessage = msg;
        emit errorOccurred(msg);
        return false; // skipped or cancelled (m_cancelled set by resolveError)
    }

    if (removeSource) {
        if (srcInfo.isDir())
            QDir(source).removeRecursively();
        else
            QFile::remove(source);
    }

    emitProgress(source);
    return true;
}

bool FileOperations::copyPaths(const QStringList &sources, const QString &destDir,
                                const ConflictResolver &resolver, QString *errorMessage) {
    m_cancelled = false;
    m_totalItems = sources.size();
    m_totalBytes = countBytes(sources);
    m_doneItems = 0;
    m_doneBytes = 0;
    m_errorBatch = ErrorAction::Retry;
    ErrorAction batchAction = ErrorAction::Retry; // sentinel meaning "ask each time"
    QDir().mkpath(destDir);

    for (const QString &source : sources) {
        waitIfPaused();
        if (m_cancelled)
            return false;
        if (!copyOne(source, destDir, /*removeSource=*/false, resolver, batchAction,
                      errorMessage)) {
            if (m_cancelled)
                return false;
            // A per-file error was already reported; keep going with the rest.
        }
        ++m_doneItems;
        emitProgress(source);
    }
    return true;
}

bool FileOperations::copyAs(const QString &source, const QString &destPath,
                             const ConflictResolver &resolver, QString *errorMessage) {
    m_cancelled = false;
    m_totalItems = 1;
    m_totalBytes = countBytes({source});
    m_doneItems = 0;
    m_doneBytes = 0;

    QFileInfo srcInfo(source);
    QString target = destPath;

    if (QFileInfo::exists(target) && QDir::cleanPath(target) != QDir::cleanPath(source)) {
        ErrorAction action = resolver ? resolver(source, target) : ErrorAction::Skip;
        if (action == ErrorAction::Cancel) {
            m_cancelled = true;
            return false;
        }
        if (action == ErrorAction::Skip || action == ErrorAction::SkipAll)
            return true;
        if (action == ErrorAction::Rename) {
            const QFileInfo ti(target);
            target = QDir(ti.absolutePath()).filePath(uniqueDestination(ti.absolutePath(),
                                                                         ti.fileName()));
        }
        // Overwrite / OverwriteAll: fall through and replace target as-is.
    }

    QDir().mkpath(QFileInfo(target).absolutePath());
    bool ok;
    if (srcInfo.isDir()) {
        ok = copyRecursively(source, target);
    } else {
        QFile::remove(target);
        ok = QFile::copy(source, target);
        if (ok)
            m_doneBytes += srcInfo.size();
    }

    if (!ok) {
        const QString msg = tr("Failed to copy %1 to %2").arg(source, target);
        if (errorMessage)
            *errorMessage = msg;
        emit errorOccurred(msg);
        return false;
    }
    ++m_doneItems;
    emitProgress(source);
    return true;
}

bool FileOperations::movePaths(const QStringList &sources, const QString &destDir,
                                const ConflictResolver &resolver, QString *errorMessage) {
    m_cancelled = false;
    m_totalItems = sources.size();
    m_totalBytes = countBytes(sources);
    m_doneItems = 0;
    m_doneBytes = 0;
    m_errorBatch = ErrorAction::Retry;
    ErrorAction batchAction = ErrorAction::Retry;
    QDir().mkpath(destDir);

    for (const QString &source : sources) {
        waitIfPaused();
        if (m_cancelled)
            return false;

        QFileInfo srcInfo(source);
        const QString destPath = QDir(destDir).filePath(srcInfo.fileName());

        // Fast path: same filesystem rename, no destination conflict. Account
        // the whole entry's bytes at once (computed before the rename, since
        // the source disappears) so the byte bar still reaches 100%.
        if (!QFileInfo::exists(destPath) && QDir::cleanPath(destPath) != QDir::cleanPath(source)) {
            const qint64 bytes = srcInfo.isDir() ? countBytes({source}) : srcInfo.size();
            if (QDir().rename(source, destPath)) {
                m_doneBytes += bytes;
                ++m_doneItems;
                emitProgress(source);
                continue;
            }
        }

        if (!copyOne(source, destDir, /*removeSource=*/true, resolver, batchAction,
                      errorMessage)) {
            if (m_cancelled)
                return false;
        }
        ++m_doneItems;
        emitProgress(source);
    }
    return true;
}

bool FileOperations::deletePaths(const QStringList &paths, bool toTrash, QString *errorMessage) {
    m_cancelled = false;
    m_totalItems = paths.size();
    m_totalBytes = 0; // bytes freed aren't a meaningful transfer measure
    m_doneItems = 0;
    m_doneBytes = 0;
    m_errorBatch = ErrorAction::Retry;

    if (toTrash) {
        QStringList args = QStringList(QStringLiteral("trash")) + paths;
        QProcess proc;
        proc.start(QStringLiteral("gio"), args);
        proc.waitForFinished(-1);
        if (proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0) {
            m_doneItems = paths.size();
            emitProgress(QString());
            return true;
        }
        // gio trash unavailable or failed: fall through to permanent delete.
    }

    for (const QString &path : paths) {
        waitIfPaused();
        if (m_cancelled)
            return false;
        QFileInfo info(path);
        while (true) {
            const bool ok = info.isDir() ? QDir(path).removeRecursively() : QFile::remove(path);
            if (ok)
                break;
            const QString msg = tr("Failed to delete %1").arg(path);
            if (resolveError(path, msg) == ErrorAction::Retry)
                continue;
            if (errorMessage)
                *errorMessage = msg;
            emit errorOccurred(msg);
            break; // skipped or cancelled
        }
        if (m_cancelled)
            return false;
        ++m_doneItems;
        emitProgress(path);
    }
    return true;
}

bool FileOperations::makeDirectory(const QString &parentDir, const QString &name,
                                    QString *errorMessage) {
    QDir dir(parentDir);
    if (dir.exists(name)) {
        const QString msg = tr("%1 already exists").arg(name);
        if (errorMessage)
            *errorMessage = msg;
        emit errorOccurred(msg);
        return false;
    }
    bool ok = dir.mkpath(name);
    if (!ok) {
        const QString msg = tr("Failed to create directory %1").arg(name);
        if (errorMessage)
            *errorMessage = msg;
        emit errorOccurred(msg);
    }
    return ok;
}

bool FileOperations::renamePath(const QString &path, const QString &newName,
                                 QString *errorMessage) {
    QFileInfo info(path);
    const QString destPath = QDir(info.absolutePath()).filePath(newName);
    if (QFileInfo::exists(destPath)) {
        const QString msg = tr("%1 already exists").arg(newName);
        if (errorMessage)
            *errorMessage = msg;
        emit errorOccurred(msg);
        return false;
    }
    bool ok = QDir().rename(path, destPath);
    if (!ok) {
        const QString msg = tr("Failed to rename %1").arg(path);
        if (errorMessage)
            *errorMessage = msg;
        emit errorOccurred(msg);
    }
    return ok;
}

bool FileOperations::createSymlinks(const QStringList &sources, const QString &destDir,
                                     QString *errorMessage) {
    m_cancelled = false;
    m_totalItems = sources.size();
    m_totalBytes = 0; // links carry no bytes
    m_doneItems = 0;
    m_doneBytes = 0;
    bool allOk = true;

    for (const QString &source : sources) {
        waitIfPaused();
        if (m_cancelled)
            return false;

        QFileInfo srcInfo(source);
        QString destPath = QDir(destDir).filePath(srcInfo.fileName());
        if (QFileInfo::exists(destPath))
            destPath = QDir(destDir).filePath(uniqueDestination(destDir, srcInfo.fileName()));

        if (!QFile::link(source, destPath)) {
            const QString msg = tr("Failed to create link for %1").arg(source);
            if (errorMessage)
                *errorMessage = msg;
            emit errorOccurred(msg);
            allOk = false;
        }
        ++m_doneItems;
        emitProgress(source);
    }
    return allOk;
}
