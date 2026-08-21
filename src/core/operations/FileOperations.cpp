#include "PathSemantics.h"
#include "FileOperations.h"
#include "TrashService.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QUuid>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <cerrno>
#endif

#include "FileInfo.h"
#include "FileProvider.h"
#include "LocalFileProvider.h"

namespace {

// Which spelling rules this filesystem follows. PathSemantics is written to be
// told rather than to guess, so that its tests can drive both.
constexpr PathFlavor pathFlavor() {
#ifdef Q_OS_WIN
    return PathFlavor::Windows;
#else
    return PathFlavor::Posix;
#endif
}

qint64 lastFileError()
{
#ifdef Q_OS_WIN
    return static_cast<qint64>(GetLastError());
#else
    return static_cast<qint64>(errno);
#endif
}

QString stagingPathFor(const QString &target, const QString &purpose)
{
    const QFileInfo targetInfo(target);
    return QDir(targetInfo.absolutePath())
        .filePath(QStringLiteral(".%1.filecommander-%2-%3")
                      .arg(targetInfo.fileName(), purpose,
                           QUuid::createUuid().toString(QUuid::WithoutBraces)));
}

bool removeLocalPath(const QString &path)
{
    const QFileInfo info(path);
    return info.isDir() && !info.isSymLink() ? QDir(path).removeRecursively()
                                             : QFile::remove(path);
}

bool renameLocalPath(const QString &source, const QString &target, qint64 *nativeCode)
{
#ifdef Q_OS_WIN
    SetLastError(ERROR_SUCCESS);
    const bool ok = MoveFileExW(reinterpret_cast<LPCWSTR>(source.utf16()),
                                reinterpret_cast<LPCWSTR>(target.utf16()),
                                MOVEFILE_WRITE_THROUGH) != 0;
#else
    errno = 0;
    const bool ok = QFile::rename(source, target);
#endif
    if (!ok && nativeCode)
        *nativeCode = lastFileError();
    return ok;
}

bool commitStagedPath(const QString &staged, const QString &target, qint64 *nativeCode)
{
    const bool targetExists = QFileInfo(target).exists() || QFileInfo(target).isSymLink();
    if (!targetExists)
        return renameLocalPath(staged, target, nativeCode);

    const QString backup = stagingPathFor(target, QStringLiteral("backup"));
    if (!renameLocalPath(target, backup, nativeCode))
        return false;
    if (renameLocalPath(staged, target, nativeCode)) {
        removeLocalPath(backup);
        return true;
    }

    qint64 ignored = 0;
    renameLocalPath(backup, target, &ignored);
    return false;
}

} // namespace

FileOperations::FileOperations(QObject *parent)
    : QObject(parent),
      m_privilegeExecutor([this](const PrivilegedOperationRequest &request) {
          return PrivilegeBroker::execute(request, [this] { return m_cancelled.load(); });
      }) {}

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

ErrorAction FileOperations::resolveError(const OperationError &error) {
    if (m_errorBatch == ErrorAction::SkipAll)
        return ErrorAction::SkipAll; // "skip all" already chosen for this batch
    const ErrorAction action = m_errorResolver ? m_errorResolver(error) : ErrorAction::Skip;
    if (action == ErrorAction::SkipAll)
        m_errorBatch = ErrorAction::SkipAll;
    if (action == ErrorAction::Cancel || action == ErrorAction::Abort)
        m_cancelled = true;
    return action;
}

ErrorAction FileOperations::resolveError(OperationType operation, const QString &sourcePath,
                                         const QString &targetPath, qint64 nativeCode,
                                         const QString &message, bool localOperation) {
    nativeCode = overrideNativeErrorForTesting(operation, sourcePath, targetPath, nativeCode);
    return resolveError(classifyNativeOperationError(operation, sourcePath, targetPath,
                                                     nativeCode, localOperation, message));
}

qint64 FileOperations::overrideNativeErrorForTesting(OperationType operation,
                                                     const QString &sourcePath,
                                                     const QString &targetPath,
                                                     qint64 nativeCode) const {
    return m_nativeErrorOverrideForTesting
        ? m_nativeErrorOverrideForTesting(operation, sourcePath, targetPath, nativeCode)
        : nativeCode;
}

FileOperations::FailureResolution
FileOperations::resolveLocalFailure(OperationError error,
                                    const PrivilegedOperationRequest &request) {
    if (validatePrivilegedOperationRequest(request).status != PrivilegeStatus::Succeeded) {
        error.elevatable = false;
        error.remote = true;
    }
    while (true) {
        const ErrorAction action = resolveError(error);
        if (action == ErrorAction::Retry)
            return FailureResolution::Retry;
        if (action != ErrorAction::Elevate)
            return FailureResolution::Failed;
        if (!error.elevatable || error.remote || !m_privilegeExecutor)
            return FailureResolution::Failed;

        const PrivilegeResult result = m_privilegeExecutor(request);
        if (result.status == PrivilegeStatus::Succeeded)
            return FailureResolution::Elevated;

        error = classifyNativeOperationError(
            error.operation, error.sourcePath, error.targetPath, result.nativeCode, true,
            result.message.isEmpty() ? tr("The administrator operation did not complete.")
                                     : result.message);
        error.elevatable = false;
    }
}

QString FileOperations::uniqueDestination(const QString &destDir, const QString &name) {
    const QString base = FileInfo::baseNameForName(name);
    const QString suffix = FileInfo::suffixForName(name);
    int n = 1;
    QString candidate = name;
    while (QFileInfo::exists(QDir(destDir).filePath(candidate))) {
        candidate = suffix.isEmpty() ? QStringLiteral("%1 (%2)").arg(base).arg(n)
                                      : QStringLiteral("%1 (%2).%3").arg(base).arg(n).arg(suffix);
        ++n;
    }
    return candidate;
}

bool FileOperations::copyInterrupted(qint64 copiedBytes, qint64 totalBytes) {
    if (m_copyChunkHookForTesting)
        m_copyChunkHookForTesting(copiedBytes, totalBytes);
    return m_cancelled.load();
}

#ifdef Q_OS_WIN
namespace {

// Win32 hands this to CopyFileExW once per copied chunk. Returning
// PROGRESS_CANCEL stops the copy where it stands *and* has Windows delete the
// half-written destination, which is exactly the guarantee we want to give the
// user for the file that was in flight when they hit 中止.
DWORD CALLBACK abortableCopyProgress(LARGE_INTEGER totalFileSize,
                                     LARGE_INTEGER totalBytesTransferred, LARGE_INTEGER,
                                     LARGE_INTEGER, DWORD, DWORD, HANDLE, HANDLE,
                                     LPVOID data) {
    auto *ops = static_cast<FileOperations *>(data);
    return ops->copyInterrupted(
               static_cast<qint64>(totalBytesTransferred.QuadPart),
               static_cast<qint64>(totalFileSize.QuadPart))
        ? PROGRESS_CANCEL
        : PROGRESS_CONTINUE;
}

} // namespace
#endif

#ifndef Q_OS_WIN
bool FileOperations::copyFileChunked(const QString &source, const QString &target,
                                     qint64 *nativeCode) {
    const auto fail = [nativeCode](int code) {
        if (nativeCode)
            *nativeCode = static_cast<qint64>(code);
        return false;
    };

    // QFile::copy refuses to overwrite; keep that contract so callers that rely
    // on it (the non-overwrite path, and the staged-copy path which generates a
    // fresh unique name) behave identically to before.
    if (QFileInfo(target).exists() || QFileInfo(target).isSymLink())
        return fail(EEXIST);

    QFile in(source);
    errno = 0;
    if (!in.open(QIODevice::ReadOnly))
        return fail(errno ? errno : EIO);
    QFile out(target);
    errno = 0;
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fail(errno ? errno : EIO);

    constexpr qint64 kChunkBytes = 1 << 20;
    QByteArray buffer(kChunkBytes, Qt::Uninitialized);
    const qint64 total = in.size();
    qint64 copied = 0;
    while (true) {
        if (copyInterrupted(copied, total)) {
            out.close();
            QFile::remove(target); // never leave a half-written file behind
            return fail(ECANCELED);
        }
        const qint64 read = in.read(buffer.data(), kChunkBytes);
        if (read < 0) {
            out.close();
            QFile::remove(target);
            return fail(EIO);
        }
        if (read == 0)
            break;
        if (out.write(buffer.constData(), read) != read) {
            out.close();
            QFile::remove(target);
            return fail(errno ? errno : EIO);
        }
        copied += read;
    }
    if (!out.flush()) {
        out.close();
        QFile::remove(target);
        return fail(errno ? errno : EIO);
    }
    out.setPermissions(in.permissions()); // QFile::copy carries these across too
    out.close();
    return true;
}
#endif

bool FileOperations::copyFilePreservingTime(const QString &source, const QString &target,
                                            bool overwrite, qint64 *nativeCode) {
    // Capture the source time before the copy: if source and target somehow
    // resolve to the same file, reading it afterwards would give the new stamp.
    const QDateTime sourceTime = QFileInfo(source).lastModified();

    if (nativeCode)
        *nativeCode = 0;
    const bool targetExists = QFileInfo(target).exists() || QFileInfo(target).isSymLink();
    const QString copyTarget = overwrite && targetExists
        ? stagingPathFor(target, QStringLiteral("copy"))
        : target;
#ifdef Q_OS_WIN
    // CopyFileExW rather than CopyFileW purely for the progress routine: it is
    // the only way to make a single large copy give up before it finishes.
    if (!CopyFileExW(reinterpret_cast<LPCWSTR>(source.utf16()),
                     reinterpret_cast<LPCWSTR>(copyTarget.utf16()), &abortableCopyProgress,
                     this, nullptr, COPY_FILE_FAIL_IF_EXISTS)) {
        const DWORD error = GetLastError();
        if (nativeCode)
            *nativeCode = static_cast<qint64>(error);
        // Windows deletes the destination itself when the copy is cancelled;
        // the belt-and-braces removal covers the case where it survives anyway
        // (and costs nothing when it doesn't).
        if (error == ERROR_REQUEST_ABORTED)
            removeLocalPath(copyTarget);
        return false;
    }
#else
    errno = 0;
    if (!copyFileChunked(source, copyTarget, nativeCode))
        return false;
#endif

    if (copyTarget != target && !commitStagedPath(copyTarget, target, nativeCode)) {
        removeLocalPath(copyTarget);
        return false;
    }

    // Best-effort restamp. QFile::copy leaves the destination dated to the
    // moment it was written, which made a just-synchronised file look NEWER
    // than its own source -- so a re-comparison reported every copied file as a
    // difference, with the arrow pointing back the way it came.
    LocalFileProvider::instance()->setModifiedTime(target, sourceTime);
    return true;
}

bool FileOperations::copyRecursively(const QString &sourceDir, const QString &destDir,
                                     qint64 *nativeCode) {
#ifdef Q_OS_WIN
    SetLastError(ERROR_SUCCESS);
#else
    errno = 0;
#endif
    if (!QDir().mkpath(destDir)) {
        if (nativeCode)
            *nativeCode = lastFileError();
        return false;
    }
    QDirIterator it(sourceDir, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
    while (it.hasNext()) {
        it.next();
        const QFileInfo entry = it.fileInfo();
        const QString target = QDir(destDir).filePath(entry.fileName());
        if (entry.isDir()) {
            if (!copyRecursively(entry.filePath(), target, nativeCode))
                return false;
        } else {
            if (!copyFilePreservingTime(entry.filePath(), target, false, nativeCode))
                return false;
            m_doneBytes += entry.size();
        }
        emitProgress(entry.filePath());
        if (m_cancelled)
            return false;
    }
    return true;
}

bool FileOperations::copyDirectorySafely(const QString &sourceDir, const QString &destDir,
                                         bool overwrite, qint64 *nativeCode) {
    const bool targetExists = QFileInfo(destDir).exists() || QFileInfo(destDir).isSymLink();
    if (targetExists && !overwrite) {
#ifdef Q_OS_WIN
        if (nativeCode)
            *nativeCode = ERROR_ALREADY_EXISTS;
#else
        if (nativeCode)
            *nativeCode = EEXIST;
#endif
        return false;
    }

    const QString copyTarget = targetExists
        ? stagingPathFor(destDir, QStringLiteral("copy"))
        : destDir;
    if (!copyRecursively(sourceDir, copyTarget, nativeCode)) {
        if (copyTarget != destDir)
            removeLocalPath(copyTarget);
        else if (m_cancelled && !targetExists)
            // Aborted part-way through a directory we created ourselves: take
            // the half-populated tree with us. (Only safe because copyTarget ==
            // destDir means the destination did not exist before we started, so
            // everything under it is ours. A tree that pre-existed is staged and
            // handled by the branch above; a *failed* -- as opposed to aborted --
            // copy still leaves its partial output alone, as it always has, so a
            // Retry has something to build on.)
            removeLocalPath(destDir);
        return false;
    }
    if (copyTarget != destDir && !commitStagedPath(copyTarget, destDir, nativeCode)) {
        removeLocalPath(copyTarget);
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
    bool overwrite = false;

    // A directory cannot be copied into itself. The copy would write into the
    // tree it is still reading and recurse into its own output, stopping only
    // when the path outgrows the filesystem -- and nothing further down notices,
    // because every individual file copy along the way is perfectly legal.
    // Observed in the wild as a move of X into X/X that ran to 1.9 GB.
    //
    // Guarded here rather than only at the UI, because the menu, the keyboard
    // commands and drag-and-drop all arrive through this one function.
    if (srcInfo.isDir() && PathSemantics::isInsideOrSame(destDir, source, pathFlavor())) {
        if (errorMessage) {
            *errorMessage = removeSource
                                ? QObject::tr("%1 cannot be moved into itself.").arg(source)
                                : QObject::tr("%1 cannot be copied into itself.").arg(source);
        }
        return false;
    }

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
        if (action != ErrorAction::OverwriteAll && action != ErrorAction::SkipAll) {
            // Local paths, so QFileInfo is the right instrument here -- but a
            // directory has no meaningful single size, so leave that unknown.
            const QFileInfo dstInfo(destPath);
            const FileConflict conflict{source, destPath,
                                        srcInfo.isDir() ? -1 : srcInfo.size(),
                                        dstInfo.isDir() ? -1 : dstInfo.size()};
            action = resolver ? resolver(conflict) : ErrorAction::Skip;
        }

        if (action == ErrorAction::OverwriteAll || action == ErrorAction::SkipAll)
            batchAction = action;

        if (action == ErrorAction::Cancel || action == ErrorAction::Abort) {
            m_cancelled = true;
            return false;
        }
        if (action == ErrorAction::Skip || action == ErrorAction::SkipAll)
            return false;
        if (action == ErrorAction::Rename)
            destPath = QDir(destDir).filePath(uniqueDestination(destDir, destName));
        else if (action == ErrorAction::Overwrite || action == ErrorAction::OverwriteAll)
            overwrite = true;
        // Overwrite / OverwriteAll: fall through and replace destPath as-is.
    }

    bool elevated = false;
    while (true) {
        bool ok;
        qint64 nativeCode = 0;
        if (srcInfo.isDir()) {
            ok = copyDirectorySafely(source, destPath, overwrite, &nativeCode);
        } else {
            ok = copyFilePreservingTime(source, destPath, overwrite, &nativeCode);
            if (ok)
                m_doneBytes += srcInfo.size();
        }
        if (ok)
            break;

        // A copy that stopped because the user asked it to is not a failure to
        // put a decision dialog in front of them: bail out silently instead of
        // asking Retry/Skip/Abort about the abort they just requested.
        if (m_cancelled)
            return false;

        const QString msg = tr("Failed to copy %1 to %2").arg(source, destPath);
        nativeCode = overrideNativeErrorForTesting(removeSource ? OperationType::Move
                                                                : OperationType::Copy,
                                                   source, destPath, nativeCode);
        OperationError operationError = classifyNativeOperationError(
            removeSource ? OperationType::Move : OperationType::Copy, source, destPath,
            nativeCode, true, msg);
        const PrivilegedOperationRequest request{
            1,
            removeSource ? PrivilegedOperationKind::Move : PrivilegedOperationKind::Copy,
            source,
            destPath,
            overwrite,
        };
        const FailureResolution resolution = resolveLocalFailure(operationError, request);
        if (resolution == FailureResolution::Retry)
            continue; // user asked to retry
        if (resolution == FailureResolution::Elevated) {
            elevated = true;
            m_doneBytes += srcInfo.size();
            break;
        }
        if (errorMessage)
            *errorMessage = msg;
        emit errorOccurred(msg);
        return false; // skipped or cancelled (m_cancelled set by resolveError)
    }

    if (removeSource && !elevated) {
        while (true) {
#ifdef Q_OS_WIN
            SetLastError(ERROR_SUCCESS);
#else
            errno = 0;
#endif
            if (removeLocalPath(source))
                break;

            qint64 nativeCode = overrideNativeErrorForTesting(
                OperationType::Delete, source, {}, lastFileError());
            const QString msg = tr("Failed to remove the source after copying %1").arg(source);
            OperationError operationError = classifyNativeOperationError(
                OperationType::Delete, source, {}, nativeCode, true, msg);
            const PrivilegedOperationRequest request{
                1, PrivilegedOperationKind::DeletePermanent, source, {}, false};
            const FailureResolution resolution = resolveLocalFailure(operationError, request);
            if (resolution == FailureResolution::Retry)
                continue;
            if (resolution == FailureResolution::Elevated)
                break;
            if (errorMessage)
                *errorMessage = msg;
            emit errorOccurred(msg);
            return false;
        }
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
    bool allOk = true;

    for (const QString &source : sources) {
        waitIfPaused();
        if (m_cancelled)
            return false;
        if (!copyOne(source, destDir, /*removeSource=*/false, resolver, batchAction,
                     errorMessage)) {
            if (m_cancelled)
                return false;
            allOk = false;
            continue;
        }
        ++m_doneItems;
        emitProgress(source);
    }
    return allOk;
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
    bool overwrite = false;

    if (QFileInfo::exists(target) && QDir::cleanPath(target) != QDir::cleanPath(source)) {
        const QFileInfo tgtInfo(target);
        const FileConflict conflict{source, target, srcInfo.isDir() ? -1 : srcInfo.size(),
                                    tgtInfo.isDir() ? -1 : tgtInfo.size()};
        ErrorAction action = resolver ? resolver(conflict) : ErrorAction::Skip;
        if (action == ErrorAction::Cancel || action == ErrorAction::Abort) {
            m_cancelled = true;
            return false;
        }
        if (action == ErrorAction::Skip || action == ErrorAction::SkipAll)
            return false;
        if (action == ErrorAction::Rename) {
            const QFileInfo ti(target);
            target = QDir(ti.absolutePath()).filePath(uniqueDestination(ti.absolutePath(),
                                                                         ti.fileName()));
        } else if (action == ErrorAction::Overwrite || action == ErrorAction::OverwriteAll) {
            overwrite = true;
        }
        // Overwrite / OverwriteAll: fall through and replace target as-is.
    }

    QDir().mkpath(QFileInfo(target).absolutePath());
    while (true) {
        bool ok;
        qint64 nativeCode = 0;
        if (srcInfo.isDir()) {
            ok = copyDirectorySafely(source, target, overwrite, &nativeCode);
        } else {
            ok = copyFilePreservingTime(source, target, overwrite, &nativeCode);
            if (ok)
                m_doneBytes += srcInfo.size();
        }
        if (ok)
            break;

        // Same reasoning as copyOne: an aborted copy reports itself as a failed
        // one, and must not surface as an error the user has to answer.
        if (m_cancelled)
            return false;

        const QString msg = tr("Failed to copy %1 to %2").arg(source, target);
        nativeCode = overrideNativeErrorForTesting(OperationType::Copy, source, target,
                                                   nativeCode);
        OperationError operationError = classifyNativeOperationError(
            OperationType::Copy, source, target, nativeCode, true, msg);
        const PrivilegedOperationRequest request{
            1, PrivilegedOperationKind::Copy, source, target, overwrite};
        const FailureResolution resolution = resolveLocalFailure(operationError, request);
        if (resolution == FailureResolution::Retry)
            continue;
        if (resolution == FailureResolution::Elevated) {
            m_doneBytes += srcInfo.size();
            break;
        }
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
    bool allOk = true;

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
            allOk = false;
            continue;
        }
        ++m_doneItems;
        emitProgress(source);
    }
    return allOk;
}

bool FileOperations::deletePaths(const QStringList &paths, bool toTrash, QString *errorMessage) {
    m_cancelled = false;
    m_totalItems = paths.size();
    m_totalBytes = 0; // bytes freed aren't a meaningful transfer measure
    m_doneItems = 0;
    m_doneBytes = 0;
    m_errorBatch = ErrorAction::Retry;

    if (toTrash) {
        bool allOk = true;
        for (const QString &path : paths) {
            waitIfPaused();
            if (m_cancelled)
                return false;
            bool removed = false;
            while (true) {
                const PlatformResult result = createTrashService()->moveToTrash({path});
                if (result.ok) {
                    removed = true;
                    break;
                }
                OperationError operationError = classifyNativeOperationError(
                    OperationType::Delete, path, {}, result.nativeCode, true, result.message);
                // The Security Amendment forbids elevating the *trash* move itself (the
                // privileged helper has no recycle primitive, and on Linux "root's trash"
                // isn't a meaningful place to land the file). Offer the one operation it
                // does support instead -- permanent delete -- exactly as real Explorer does
                // when recycling a protected file needs administrator approval. Say so
                // plainly so elevating never silently skips the Recycle Bin.
                if (operationError.elevatable) {
                    operationError.message = tr("%1\nDeleting as administrator bypasses the "
                                                 "Recycle Bin and cannot be undone.")
                                                  .arg(operationError.message);
                }
                const PrivilegedOperationRequest request{
                    1, PrivilegedOperationKind::DeletePermanent, path, {}, false};
                const FailureResolution resolution = resolveLocalFailure(operationError, request);
                if (resolution == FailureResolution::Retry)
                    continue;
                if (resolution == FailureResolution::Elevated) {
                    removed = true;
                    break;
                }
                if (errorMessage)
                    *errorMessage = operationError.message;
                emit errorOccurred(operationError.message);
                break; // skipped, cancelled, or aborted
            }
            if (m_cancelled)
                return false;
            if (!removed) {
                allOk = false;
                continue;
            }
            ++m_doneItems;
            emitProgress(path);
        }
        return allOk;
    }

    bool allOk = true;
    for (const QString &path : paths) {
        waitIfPaused();
        if (m_cancelled)
            return false;
        QFileInfo info(path);
        bool removed = false;
        while (true) {
            const bool ok = info.isDir() ? QDir(path).removeRecursively() : QFile::remove(path);
            if (ok) {
                removed = true;
                break;
            }
            const QString msg = tr("Failed to delete %1").arg(path);
            qint64 nativeCode = 0;
#ifdef Q_OS_WIN
            nativeCode = static_cast<qint64>(GetLastError());
#else
            nativeCode = static_cast<qint64>(errno);
#endif
            nativeCode = overrideNativeErrorForTesting(OperationType::Delete, path, {},
                                                       nativeCode);
            OperationError operationError = classifyNativeOperationError(
                OperationType::Delete, path, {}, nativeCode, true, msg);
            const PrivilegedOperationRequest request{
                1, PrivilegedOperationKind::DeletePermanent, path, {}, false};
            const FailureResolution resolution = resolveLocalFailure(operationError, request);
            if (resolution == FailureResolution::Retry)
                continue;
            if (resolution == FailureResolution::Elevated) {
                removed = true;
                break;
            }
            if (errorMessage)
                *errorMessage = msg;
            emit errorOccurred(msg);
            break; // skipped or cancelled
        }
        if (m_cancelled)
            return false;
        if (!removed) {
            allOk = false;
            continue;
        }
        ++m_doneItems;
        emitProgress(path);
    }
    return allOk;
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
    const QString targetPath = dir.filePath(name);
    while (true) {
        if (dir.mkpath(name))
            return true;

        const QString msg = tr("Failed to create directory %1").arg(name);
        qint64 nativeCode = 0;
#ifdef Q_OS_WIN
        nativeCode = static_cast<qint64>(GetLastError());
#else
        nativeCode = static_cast<qint64>(errno);
#endif
        nativeCode = overrideNativeErrorForTesting(OperationType::Mkdir, {}, targetPath,
                                                   nativeCode);
        OperationError operationError = classifyNativeOperationError(
            OperationType::Mkdir, {}, targetPath, nativeCode, true, msg);
        const PrivilegedOperationRequest request{
            1, PrivilegedOperationKind::Mkdir, {}, targetPath, false};
        const FailureResolution resolution = resolveLocalFailure(operationError, request);
        if (resolution == FailureResolution::Retry)
            continue;
        if (resolution == FailureResolution::Elevated)
            return true;
        if (errorMessage)
            *errorMessage = msg;
        emit errorOccurred(msg);
        return false;
    }
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
    while (true) {
        if (QDir().rename(path, destPath))
            return true;

        const QString msg = tr("Failed to rename %1").arg(path);
        qint64 nativeCode = 0;
#ifdef Q_OS_WIN
        nativeCode = static_cast<qint64>(GetLastError());
#else
        nativeCode = static_cast<qint64>(errno);
#endif
        nativeCode = overrideNativeErrorForTesting(OperationType::Rename, path, destPath,
                                                   nativeCode);
        OperationError operationError = classifyNativeOperationError(
            OperationType::Rename, path, destPath, nativeCode, true, msg);
        const PrivilegedOperationRequest request{
            1, PrivilegedOperationKind::Rename, path, destPath, false};
        const FailureResolution resolution = resolveLocalFailure(operationError, request);
        if (resolution == FailureResolution::Retry)
            continue;
        if (resolution == FailureResolution::Elevated)
            return true;
        if (errorMessage)
            *errorMessage = msg;
        emit errorOccurred(msg);
        return false;
    }
}

bool FileOperations::makeProviderDirectory(FileProvider *dst, const QString &parentDir,
                                           const QString &name, QString *errorMessage) {
    const QString full = joinPath(parentDir, name);
    if (dst->exists(full)) {
        const QString msg = tr("%1 already exists").arg(name);
        if (errorMessage)
            *errorMessage = msg;
        emit errorOccurred(msg);
        return false;
    }
    if (!dst->mkdir(full)) {
        const QString msg = tr("Failed to create directory %1").arg(name);
        if (errorMessage)
            *errorMessage = msg;
        emit errorOccurred(msg);
        return false;
    }
    return true;
}

bool FileOperations::removeProviderTree(FileProvider *provider, const QString &path,
                                        QString *errorMessage) {
    waitIfPaused();
    if (m_cancelled)
        return false;

    // Depth-first: a directory can only be removed once emptied, so recurse into
    // its children before removing the node itself.
    if (provider->isDir(path)) {
        const QVector<FileInfo> entries = provider->list(path, /*showHidden=*/true);
        for (const FileInfo &entry : entries) {
            if (m_cancelled)
                return false;
            if (!removeProviderTree(provider, entry.path(), errorMessage))
                return false;
        }
    }

    while (!provider->remove(path)) {
        const QString msg = tr("Failed to delete %1").arg(path);
        if (resolveError(OperationType::Delete, path, {}, 0, msg, false) ==
            ErrorAction::Retry)
            continue;
        if (errorMessage)
            *errorMessage = msg;
        emit errorOccurred(msg);
        return false; // skipped or cancelled
    }
    return true;
}

bool FileOperations::deleteProviderPaths(FileProvider *provider, const QStringList &paths,
                                         QString *errorMessage) {
    m_cancelled = false;
    m_totalItems = paths.size();
    m_totalBytes = 0; // bytes freed aren't a meaningful transfer measure
    m_doneItems = 0;
    m_doneBytes = 0;
    m_errorBatch = ErrorAction::Retry;

    bool allOk = true;
    for (const QString &path : paths) {
        waitIfPaused();
        if (m_cancelled)
            return false;
        if (!removeProviderTree(provider, path, errorMessage)) {
            if (m_cancelled)
                return false;
            allOk = false; // this entry was skipped; keep deleting the rest
            continue;
        }
        ++m_doneItems;
        emitProgress(path);
    }
    return allOk;
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

        bool linked = false;
        while (!linked) {
            if (QFile::link(source, destPath)) {
                linked = true;
                break;
            }
            const QString msg = tr("Failed to create link for %1").arg(source);
            qint64 nativeCode = 0;
#ifdef Q_OS_WIN
            nativeCode = static_cast<qint64>(GetLastError());
#else
            nativeCode = static_cast<qint64>(errno);
#endif
            nativeCode = overrideNativeErrorForTesting(OperationType::Copy, source, destPath,
                                                       nativeCode);
            OperationError operationError = classifyNativeOperationError(
                OperationType::Copy, source, destPath, nativeCode, true, msg);
            const PrivilegedOperationRequest request{
                1, PrivilegedOperationKind::Symlink, source, destPath, false};
            const FailureResolution resolution = resolveLocalFailure(operationError, request);
            if (resolution == FailureResolution::Retry)
                continue;
            if (resolution == FailureResolution::Elevated)
                linked = true;
            if (linked)
                break;
            if (errorMessage)
                *errorMessage = msg;
            emit errorOccurred(msg);
            allOk = false;
            break;
        }
        if (!linked)
            continue;
        ++m_doneItems;
        emitProgress(source);
    }
    return allOk;
}

// --- Cross-provider transfer (local<->remote) with resume ------------------

QString FileOperations::joinPath(const QString &dir, const QString &name) {
    // Both the local and SFTP providers speak '/'-separated paths, so a single
    // join works for either side of the transfer.
    if (dir.endsWith(QLatin1Char('/')))
        return dir + name;
    return dir + QLatin1Char('/') + name;
}

QString FileOperations::lastComponent(const QString &path) {
    QString p = path;
    while (p.size() > 1 && p.endsWith(QLatin1Char('/')))
        p.chop(1);
    const int slash = p.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? p : p.mid(slash + 1);
}

qint64 FileOperations::providerFileSize(FileProvider *provider, const QString &path) {
    // Read-open the file purely to learn its size, then release the handle. Used
    // for the top-level file (which has no cached FileInfo) and for probing a
    // partial destination before a resume.
    FileHandle *handle = provider->openRead(path);
    if (!handle)
        return -1;
    const qint64 size = provider->handleSize(handle);
    provider->closeHandle(handle);
    return size;
}

QDateTime FileOperations::providerFileModified(FileProvider *provider, const QString &path) {
    // There is no per-file stat in the FileProvider interface, so the file's own
    // entry is picked out of its parent's listing -- the same listing the model
    // already relies on for sizes and times. Returns an invalid QDateTime when
    // the entry can't be found, which callers treat as "don't restamp".
    const QString parent = provider->parentPath(path);
    if (parent.isEmpty())
        return {};
    const QString name = path.section(QLatin1Char('/'), -1);
    const QVector<FileInfo> entries = provider->list(parent, /*showHidden=*/true);
    for (const FileInfo &entry : entries) {
        if (entry.name() == name)
            return entry.modified();
    }
    return {};
}

qint64 FileOperations::providerTreeBytes(FileProvider *src, const QString &path) {
    if (!src->isDir(path))
        return qMax<qint64>(0, providerFileSize(src, path));

    qint64 total = 0;
    // A directory listing already carries each child's size, so we only open a
    // handle for the (single) top-level file case above — not for every leaf.
    const QVector<FileInfo> entries = src->list(path, /*showHidden=*/true);
    for (const FileInfo &entry : entries) {
        if (entry.isDir())
            total += providerTreeBytes(src, entry.path());
        else
            total += qMax<qint64>(0, entry.size());
    }
    return total;
}

qint64 FileOperations::countProviderBytes(FileProvider *src, const QStringList &paths) {
    qint64 total = 0;
    for (const QString &path : paths)
        total += providerTreeBytes(src, path);
    return total;
}

QString FileOperations::uniqueProviderDestination(FileProvider *dst, const QString &destPath) {
    if (!dst->exists(destPath))
        return destPath;

    const QString parent = dst->parentPath(destPath);
    const QString dir = parent.isEmpty() ? QStringLiteral("/") : parent;
    const QString name = lastComponent(destPath);
    const QString base = FileInfo::baseNameForName(name);
    const QString suffix = FileInfo::suffixForName(name);

    int n = 1;
    QString candidate;
    do {
        const QString newName = suffix.isEmpty() ? QStringLiteral("%1 (%2)").arg(base).arg(n)
                                                 : QStringLiteral("%1 (%2).%3")
                                                       .arg(base)
                                                       .arg(n)
                                                       .arg(suffix);
        candidate = joinPath(dir, newName);
        ++n;
    } while (dst->exists(candidate));
    return candidate;
}

bool FileOperations::streamCopy(FileProvider *src, const QString &srcPath, FileProvider *dst,
                                const QString &destPath, bool truncate, qint64 startOffset,
                                QString *failMsg, const QDateTime &sourceTime) {
    // Roll back progress accounting if the attempt fails so a retry starts from
    // a clean byte count rather than double-counting.
    const qint64 doneBytesAtStart = m_doneBytes;
    const auto writeFailureMessage = [this](FileHandle::StreamError error, const QString &detail,
                                            const QString &path, bool finalCommit) {
        switch (error) {
        case FileHandle::StreamError::NoSpace:
            return tr("The destination has no space for %1").arg(path);
        case FileHandle::StreamError::PermissionDenied:
            return tr("You do not have permission to write %1").arg(path);
        case FileHandle::StreamError::ConnectionLost:
            return tr("Connection to the server was lost while transferring %1").arg(path);
        case FileHandle::StreamError::Other:
            if (!detail.isEmpty()) {
                return finalCommit ? tr("Upload of %1 did not complete: %2").arg(path, detail)
                                   : tr("Write error on %1: %2").arg(path, detail);
            }
            break;
        case FileHandle::StreamError::None:
            break;
        }
        return finalCommit ? tr("Upload of %1 did not complete").arg(path)
                           : tr("Write error on %1").arg(path);
    };

    FileHandle *in = src->openRead(srcPath);
    if (!in) {
        *failMsg = tr("Failed to open %1 for reading").arg(srcPath);
        return false;
    }
    FileHandle *out = dst->openWrite(destPath, truncate);
    if (!out) {
        src->closeHandle(in);
        *failMsg = tr("Failed to open %1 for writing").arg(destPath);
        return false;
    }

    // Tell the destination how many bytes are coming, where the source can say.
    // A streamed HTTP PUT must commit to Content-Length vs chunked before the
    // first byte leaves, and only this loop knows the total; -1 (source size
    // unknown) leaves the backend on its own fallback. On a resume only the
    // remaining tail is sent, so the declared length is the tail, not the file.
    // Backends that don't care ignore this entirely.
    const qint64 srcSize = src->handleSize(in);
    const qint64 expectedBytes =
        srcSize >= 0 ? qMax<qint64>(0, srcSize - qMax<qint64>(0, startOffset)) : -1;

    bool ok = true;
    if (srcSize >= 0 && startOffset > srcSize) {
        ok = false;
        *failMsg = tr("Source shrank before resuming transfer of %1").arg(destPath);
    } else {
        dst->setExpectedWriteSize(out, expectedBytes);
    }
    qint64 remainingBytes = expectedBytes;
    if (startOffset > 0) {
        // Resume: line both handles up at the byte where the last run stopped.
        if (!src->seek(in, startOffset) || !dst->seek(out, startOffset)) {
            ok = false;
            *failMsg = tr("Failed to resume transfer of %1").arg(destPath);
        } else {
            m_doneBytes += startOffset;
            emitProgress(srcPath);
        }
    }

    char buffer[64 * 1024];
    while (ok) {
        // Pause/cancel are honoured mid-file (not just per file) so a large
        // remote transfer can be interrupted promptly.
        waitIfPaused();
        if (m_cancelled) {
            ok = false;
            break;
        }

        if (remainingBytes == 0)
            break;

        const qint64 maxRead = remainingBytes > 0 ? qMin<qint64>(sizeof(buffer), remainingBytes)
                                                   : sizeof(buffer);
        const qint64 got = src->read(in, buffer, maxRead);
        if (got < 0) {
            ok = false;
            *failMsg = tr("Read error on %1").arg(srcPath);
            break;
        }
        if (got == 0) {
            if (remainingBytes > 0) {
                ok = false;
                *failMsg = tr("Unexpected end of %1").arg(srcPath);
            }
            break;
        }
        if (got > maxRead) {
            ok = false;
            *failMsg = tr("Read error on %1").arg(srcPath);
            break;
        }

        // A single write may accept fewer bytes than offered (common on SFTP),
        // so loop until the whole chunk is out.
        qint64 written = 0;
        while (written < got) {
            const qint64 w = dst->write(out, buffer + written, got - written);
            if (w <= 0) {
                ok = false;
                *failMsg = writeFailureMessage(out->streamError(), out->streamErrorDetail(), destPath, false);
                break;
            }
            written += w;
        }
        if (!ok)
            break;
        if (remainingBytes > 0)
            remainingBytes -= got;
        m_doneBytes += got;
        emitProgress(srcPath);
    }

    if (ok && expectedBytes == 0 && dst->write(out, "", 0) < 0) {
        ok = false;
        *failMsg = writeFailureMessage(out->streamError(), out->streamErrorDetail(), destPath, false);
    }
    if (ok && remainingBytes > 0) {
        ok = false;
        *failMsg = tr("Unexpected end of %1").arg(srcPath);
    }
    if (ok && expectedBytes >= 0) {
        char extraByte;
        const qint64 extra = src->read(in, &extraByte, 1);
        if (extra < 0) {
            ok = false;
            *failMsg = tr("Read error on %1").arg(srcPath);
        } else if (extra > 0) {
            ok = false;
            *failMsg = tr("Source changed during transfer of %1").arg(srcPath);
        }
    }
    if (ok && srcSize >= 0) {
        const qint64 finalSize = src->handleSize(in);
        if (finalSize >= 0 && finalSize != srcSize) {
            ok = false;
            *failMsg = tr("Source changed during transfer of %1").arg(srcPath);
        }
    }

    src->closeHandle(in);
    // A streamed upload (FTP/WebDAV) only learns the real server-side result
    // when its transfer thread finishes here, at close time -- so even after a
    // clean read/write loop the commit can still fail (disk full, dropped link,
    // permission). Treat that as a failed transfer rather than a false success.
    const CloseHandleResult closeResult = dst->closeHandleResult(out);
    if (ok && !closeResult.committed) {
        ok = false;
        *failMsg = writeFailureMessage(closeResult.error, closeResult.detail, destPath, true);
    }
    if (!ok) {
        m_doneBytes = doneBytesAtStart;
        return false;
    }

    // Carry the source's modification time onto the copy, so a transferred file
    // doesn't come out looking newer than the original it was made from. Purely
    // best-effort: backends that can't set times (and those whose support has
    // not been verified against a real server) return false, which leaves the
    // long-standing behaviour untouched and must NOT turn a completed transfer
    // into a failure.
    // Prefer the time the caller already had (from the directory listing that
    // enumerated this file); only fall back to a lookup when it wasn't supplied.
    const QDateTime stamp =
        sourceTime.isValid() ? sourceTime : providerFileModified(src, srcPath);
    if (stamp.isValid())
        dst->setModifiedTime(destPath, stamp);

    return true;
}

FileOperations::FileResult
FileOperations::transferFile(FileProvider *src, const QString &srcPath, FileProvider *dst,
                             const QString &destPath, const ConflictResolver &resolver,
                             ErrorAction &batchAction, QString *errorMessage,
                             const QDateTime &sourceTime) {
    QString target = destPath;

    while (true) {
        bool truncate = true;    // overwrite from scratch unless resuming
        qint64 startOffset = 0;  // resume point

        if (dst->exists(target)) {
            const qint64 srcSize = providerFileSize(src, srcPath);
            const qint64 dstSize = providerFileSize(dst, target);

            // A shorter destination is a partial copy to resume; anything else
            // occupying the target -- the same size, larger, or a zero-length
            // stand-in -- goes to conflict resolution. Equal size is NOT "done":
            // two files can match in size and differ in content, and silently
            // skipping them would leave the wrong bytes while reporting success.
            if (dst->supportsWriteResume() && dstSize > 0 && srcSize > 0 && dstSize < srcSize) {
                // A partial copy is present: resume from its end rather than
                // restarting (断点续传).
                truncate = false;
                startOffset = dstSize;
            } else {
                // Whatever occupies the target is not a resumable partial of
                // this source: ask, rather than guess.
                ErrorAction action = batchAction;
                if (action != ErrorAction::OverwriteAll && action != ErrorAction::SkipAll) {
                    // Both sizes are already in hand from the resume probe above,
                    // read through the providers that own the two files. Nothing
                    // downstream can obtain them again: these paths belong to a
                    // server, where a QFileInfo means a same-named local file.
                    const FileConflict conflict{srcPath, target, srcSize, dstSize};
                    action = resolver ? resolver(conflict) : ErrorAction::Skip;
                }
                if (action == ErrorAction::OverwriteAll || action == ErrorAction::SkipAll)
                    batchAction = action;

                if (action == ErrorAction::Cancel || action == ErrorAction::Abort) {
                    m_cancelled = true;
                    return FileResult::Failed;
                }
                if (action == ErrorAction::Skip || action == ErrorAction::SkipAll)
                    return FileResult::Skipped;
                if (action == ErrorAction::Rename) {
                    target = uniqueProviderDestination(dst, target);
                    truncate = true;
                    startOffset = 0;
                }
                // Overwrite / OverwriteAll: truncate stays true.
            }
        }

        QString failMsg;
        if (streamCopy(src, srcPath, dst, target, truncate, startOffset, &failMsg, sourceTime))
            return FileResult::Done;
        if (m_cancelled)
            return FileResult::Failed;

        if (resolveError(OperationType::Copy, srcPath, target, 0, failMsg, false) ==
            ErrorAction::Retry)
            continue; // user asked to retry the whole file
        if (errorMessage)
            *errorMessage = failMsg;
        emit errorOccurred(failMsg);
        return FileResult::Failed;
    }
}

bool FileOperations::transferEntry(FileProvider *src, const QString &srcPath, FileProvider *dst,
                                   const QString &destPath, bool removeSource,
                                   const ConflictResolver &resolver, ErrorAction &batchAction,
                                   QString *errorMessage, const QDateTime &sourceTime) {
    waitIfPaused();
    if (m_cancelled)
        return false;

    if (src->isDir(srcPath)) {
        // Recreate the directory on the destination, then recurse its entries.
        // mkdir failing because the directory already exists (a merge into an
        // existing folder) is fine; a genuine failure must abort this subtree,
        // otherwise every child below silently fails with no root cause shown.
        if (!dst->mkdir(destPath) && !dst->isDir(destPath)) {
            const QString msg = tr("Failed to create directory %1").arg(destPath);
            if (errorMessage)
                *errorMessage = msg;
            emit errorOccurred(msg);
            return false;
        }
        const QVector<FileInfo> entries = src->list(srcPath, /*showHidden=*/true);
        for (const FileInfo &entry : entries) {
            if (m_cancelled)
                return false;
            const QString childDest = joinPath(destPath, entry.name());
            // The listing above already carries each child's timestamp, so hand
            // it down rather than making the child re-list this directory to
            // rediscover it.
            if (!transferEntry(src, entry.path(), dst, childDest, removeSource, resolver,
                               batchAction, errorMessage, entry.modified()))
                return false;
        }
        // A move removes the (now-empty) source directory once its contents have
        // all been transferred. If it can't (e.g. a child was skipped so the dir
        // isn't empty), surface it rather than silently leaving a half-move.
        if (removeSource && !src->remove(srcPath)) {
            const QString msg = tr("Moved contents but could not remove source %1").arg(srcPath);
            if (errorMessage)
                *errorMessage = msg;
            emit errorOccurred(msg);
        }
        emitProgress(srcPath);
        return true;
    }

    const FileResult result =
        transferFile(src, srcPath, dst, destPath, resolver, batchAction, errorMessage, sourceTime);
    if (result == FileResult::Failed)
        return false;
    // Only drop the source for a genuine transfer, never for a skipped file.
    if (removeSource && result == FileResult::Done && !src->remove(srcPath)) {
        const QString msg = tr("Copied but could not remove source %1").arg(srcPath);
        if (errorMessage)
            *errorMessage = msg;
        emit errorOccurred(msg);
    }
    return true;
}

FileOperations::MoveOutcome FileOperations::tryServerSideMove(FileProvider *provider,
                                                              const QString &srcPath,
                                                              const QString &destPath) {
    switch (provider->moveTo(srcPath, destPath)) {
    case FileProvider::RenameResult::Ok:
        return MoveOutcome::Moved;
    case FileProvider::RenameResult::AlreadyExists:
        return MoveOutcome::Occupied;
    case FileProvider::RenameResult::Unsupported:
    case FileProvider::RenameResult::Failed:
        // Both mean "streaming has to do it". A backend cannot reliably tell a
        // refusal from a fault (SFTP returns one code for both), and every
        // implementation guarantees a non-Ok answer left the source untouched,
        // so retreating is always safe. Nothing is reported to the user: this
        // path exists to be faster, never to introduce a new way to fail.
        return MoveOutcome::Unavailable;
    }
    return MoveOutcome::Unavailable;
}

bool FileOperations::copyAcrossProviders(FileProvider *src, const QStringList &sources,
                                         FileProvider *dst, const QString &destDir,
                                         bool removeSource, const ConflictResolver &resolver,
                                         QString *errorMessage) {
    m_cancelled = false;
    m_totalItems = sources.size();
    m_doneItems = 0;
    m_doneBytes = 0;
    m_errorBatch = ErrorAction::Retry;
    ErrorAction batchAction = ErrorAction::Retry; // sentinel: ask each time

    if (!src || !dst || !src->canStream() || !dst->canStream()) {
        const QString msg = tr("This transfer is not supported by the backend");
        if (errorMessage)
            *errorMessage = msg;
        emit errorOccurred(msg);
        return false;
    }

    m_totalBytes = countProviderBytes(src, sources);
    dst->mkdir(destDir);

    // Whether the server-side move is still worth trying for this batch. The
    // first Unsupported answer settles it: on SMB a move across shares is
    // refused for every entry alike, and a 500-file directory should not pay
    // 500 pointless round trips to be told so 500 times. Deliberately a local,
    // per-operation flag rather than anything cached across operations -- the
    // server's layout can change between two moves.
    bool serverMoveViable = removeSource && src == dst;
    bool allOk = true;

    for (const QString &source : sources) {
        waitIfPaused();
        if (m_cancelled)
            return false;

        const QString destPath = joinPath(destDir, lastComponent(source));

        if (serverMoveViable) {
            const MoveOutcome outcome = tryServerSideMove(src, source, destPath);
            if (outcome == MoveOutcome::Moved) {
                // The whole entry (file or tree) relocated in one call, so no
                // bytes crossed the wire. Credit them anyway, or the byte bar
                // would stall at 0% through the fastest move we can do.
                m_doneBytes += providerTreeBytes(src, destPath);
                ++m_doneItems;
                emitProgress(source);
                continue;
            }
            if (outcome == MoveOutcome::Unavailable)
                serverMoveViable = false; // stop asking for the rest of this batch
            // Occupied: this one entry needs conflict resolution, which
            // transferEntry runs below. Later entries may still move cleanly,
            // so the flag stays on.
        }

        if (!transferEntry(src, source, dst, destPath, removeSource, resolver, batchAction,
                           errorMessage)) {
            if (m_cancelled)
                return false;
            allOk = false;
            continue;
        }
        ++m_doneItems;
        emitProgress(source);
    }
    return allOk;
}
