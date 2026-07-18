#include "FileOperations.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>

FileOperations::FileOperations(QObject *parent) : QObject(parent) {}

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
        }
        emit progress(0, 0, entry.filePath());
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

    if (QFileInfo::exists(destPath) && QDir::cleanPath(destPath) != QDir::cleanPath(source)) {
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

    bool ok;
    if (srcInfo.isDir()) {
        ok = copyRecursively(source, destPath);
    } else {
        QFile::remove(destPath);
        ok = QFile::copy(source, destPath);
    }

    if (!ok) {
        const QString msg = tr("Failed to copy %1 to %2").arg(source, destPath);
        if (errorMessage)
            *errorMessage = msg;
        emit errorOccurred(msg);
        return false;
    }

    if (removeSource) {
        if (srcInfo.isDir())
            QDir(source).removeRecursively();
        else
            QFile::remove(source);
    }

    emit progress(0, 0, source);
    return true;
}

bool FileOperations::copyPaths(const QStringList &sources, const QString &destDir,
                                const ConflictResolver &resolver, QString *errorMessage) {
    m_cancelled = false;
    const qint64 total = countEntries(sources);
    qint64 done = 0;
    ErrorAction batchAction = ErrorAction::Retry; // sentinel meaning "ask each time"
    QDir().mkpath(destDir);

    for (const QString &source : sources) {
        if (m_cancelled)
            return false;
        if (!copyOne(source, destDir, /*removeSource=*/false, resolver, batchAction,
                      errorMessage)) {
            if (m_cancelled)
                return false;
            // A per-file error was already reported; keep going with the rest.
        }
        ++done;
        emit progress(done, total, source);
    }
    return true;
}

bool FileOperations::movePaths(const QStringList &sources, const QString &destDir,
                                const ConflictResolver &resolver, QString *errorMessage) {
    m_cancelled = false;
    const qint64 total = countEntries(sources);
    qint64 done = 0;
    ErrorAction batchAction = ErrorAction::Retry;
    QDir().mkpath(destDir);

    for (const QString &source : sources) {
        if (m_cancelled)
            return false;

        QFileInfo srcInfo(source);
        const QString destPath = QDir(destDir).filePath(srcInfo.fileName());

        // Fast path: same filesystem rename, no destination conflict.
        if (!QFileInfo::exists(destPath) && QDir().rename(source, destPath)) {
            emit progress(++done, total, source);
            continue;
        }

        if (!copyOne(source, destDir, /*removeSource=*/true, resolver, batchAction,
                      errorMessage)) {
            if (m_cancelled)
                return false;
        }
        ++done;
        emit progress(done, total, source);
    }
    return true;
}

bool FileOperations::deletePaths(const QStringList &paths, bool toTrash, QString *errorMessage) {
    m_cancelled = false;
    const qint64 total = countEntries(paths);
    qint64 done = 0;

    if (toTrash) {
        QStringList args = QStringList(QStringLiteral("trash")) + paths;
        QProcess proc;
        proc.start(QStringLiteral("gio"), args);
        proc.waitForFinished(-1);
        if (proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0) {
            emit progress(paths.size(), paths.size(), QString());
            return true;
        }
        // gio trash unavailable or failed: fall through to permanent delete.
    }

    for (const QString &path : paths) {
        if (m_cancelled)
            return false;
        QFileInfo info(path);
        bool ok = info.isDir() ? QDir(path).removeRecursively() : QFile::remove(path);
        if (!ok) {
            const QString msg = tr("Failed to delete %1").arg(path);
            if (errorMessage)
                *errorMessage = msg;
            emit errorOccurred(msg);
        }
        ++done;
        emit progress(done, total, path);
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
