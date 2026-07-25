#include "SyncScanner.h"

#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMap>
#include <QSet>
#include <algorithm>

#include "FileProvider.h"
#include "LocalFileProvider.h"

namespace {

// Joins a base directory and a relative path, tolerating an empty relative part
// (the tree root). Kept separate from QDir::filePath so an empty `rel` yields
// the base unchanged rather than a trailing slash.
QString joinPath(const QString &base, const QString &rel) {
    if (rel.isEmpty())
        return base;
    return base + QLatin1Char('/') + rel;
}

} // namespace

SyncScanner::SyncScanner(QString leftDir, FileProvider *leftProvider, QString rightDir,
                          FileProvider *rightProvider, bool recursive,
                          std::shared_ptr<std::atomic<bool>> cancel, quint64 scanId)
    : m_leftDir(std::move(leftDir)), m_leftProvider(leftProvider),
      m_rightDir(std::move(rightDir)), m_rightProvider(rightProvider), m_recursive(recursive),
      m_cancel(std::move(cancel)), m_scanId(scanId) {
    // Results cross a thread boundary as a queued signal argument, which Qt can
    // only marshal for a type registered at runtime -- without this it drops the
    // emission with a console warning and the view silently stays empty.
    static const int once = []() {
        qRegisterMetaType<QVector<SyncEntry>>("QVector<SyncEntry>");
        return 0;
    }();
    Q_UNUSED(once);
}

bool SyncScanner::listSide(FileProvider *provider, const QString &baseDir, const QString &relDir,
                            QVector<FileInfo> *files, QStringList *subDirs) const {
    const QString absDir = joinPath(baseDir, relDir);
    LocalFileProvider *local = LocalFileProvider::instance();

    if (!provider || provider == local) {
        // Local fast path: QDirIterator over a single level. Going through the
        // provider here would work, but this avoids building a FileInfo (and its
        // lazy MIME machinery) for entries we only need size/mtime from.
        QDir dir(absDir);
        if (!dir.exists())
            return false;
        const QFileInfoList infos =
            dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden |
                              QDir::System);
        for (const QFileInfo &fi : infos) {
            // Symlinked directories are not descended: a link pointing at an
            // ancestor would otherwise walk forever.
            if (fi.isDir()) {
                if (!fi.isSymLink())
                    subDirs->append(fi.fileName());
            } else {
                files->append(FileInfo::fromFields(fi.absoluteFilePath(), fi.fileName(), fi.size(),
                                                    fi.lastModified(), /*isDir=*/false,
                                                    fi.permissions()));
            }
        }
        return true;
    }

    // Remote backend. This call blocks on network I/O and serialises on the
    // provider's own mutex, exactly as FileSystemModel::directorySize does when
    // it walks a remote tree from a worker thread.
    const QVector<FileInfo> entries = provider->list(absDir, /*showHidden=*/true);
    if (entries.isEmpty() && !provider->exists(absDir))
        return false;

    for (const FileInfo &e : entries) {
        if (e.isParentEntry())
            continue;
        if (e.isDir()) {
            if (!e.isSymLink())
                subDirs->append(e.name());
        } else {
            files->append(e);
        }
    }
    return true;
}

void SyncScanner::process() {
    QStringList pending;   // relative directories still to visit
    pending.append(QString());

    QVector<SyncEntry> batch;
    int scanned = 0;
    QElapsedTimer flushTimer;
    flushTimer.start();

    while (!pending.isEmpty()) {
        if (cancelled())
            break;

        const QString relDir = pending.takeFirst();
        emit progress(m_scanId, scanned, relDir);

        QVector<FileInfo> leftFiles, rightFiles;
        QStringList leftSubs, rightSubs;

        // Strictly serial: left, then right. Never concurrent -- see the header
        // for why this is a correctness requirement and not a style choice.
        listSide(m_leftProvider, m_leftDir, relDir, &leftFiles, &leftSubs);
        if (cancelled())
            break;
        listSide(m_rightProvider, m_rightDir, relDir, &rightFiles, &rightSubs);
        if (cancelled())
            break;

        // Pair this directory's files by name and classify them immediately, so
        // what we emit is already final.
        QMap<QString, const FileInfo *> leftByName, rightByName;
        for (const FileInfo &f : leftFiles)
            leftByName.insert(f.name(), &f);
        for (const FileInfo &f : rightFiles)
            rightByName.insert(f.name(), &f);

        QStringList names;
        names.reserve(leftByName.size() + rightByName.size());
        for (auto it = leftByName.constBegin(); it != leftByName.constEnd(); ++it)
            names.append(it.key());
        for (auto it = rightByName.constBegin(); it != rightByName.constEnd(); ++it) {
            if (!leftByName.contains(it.key()))
                names.append(it.key());
        }
        std::sort(names.begin(), names.end());

        for (const QString &name : names) {
            const FileInfo *l = leftByName.value(name, nullptr);
            const FileInfo *r = rightByName.value(name, nullptr);

            SyncEntry entry;
            entry.relativePath = relDir.isEmpty() ? name : relDir + QLatin1Char('/') + name;
            if (l) {
                entry.leftSize = l->size();
                entry.leftModified = l->modified();
            }
            if (r) {
                entry.rightSize = r->size();
                entry.rightModified = r->modified();
            }
            entry.status = DirectorySync::classify(entry.leftSize, entry.leftModified, l != nullptr,
                                                    entry.rightSize, entry.rightModified,
                                                    r != nullptr);
            batch.append(entry);
            ++scanned;

            if (batch.size() >= kBatchSize) {
                if (cancelled())
                    break;
                emit entriesReady(m_scanId, batch);
                batch.clear();
                flushTimer.restart();
                emit progress(m_scanId, scanned, relDir);
            }
        }

        // Flush on the clock rather than at every directory boundary: a deep tree
        // of small directories would otherwise emit a signal per directory (see
        // kFlushIntervalMs). A partial batch simply carries into the next one.
        if (!batch.isEmpty() && !cancelled() && flushTimer.elapsed() >= kFlushIntervalMs) {
            emit entriesReady(m_scanId, batch);
            batch.clear();
            flushTimer.restart();
        }
        emit progress(m_scanId, scanned, relDir);

        if (!m_recursive)
            continue;

        // Queue the union of both sides' subdirectories, so a folder that only
        // exists on one side is still descended (its files show up as
        // left-only / right-only).
        QStringList subs = leftSubs;
        QSet<QString> seen(leftSubs.constBegin(), leftSubs.constEnd());
        for (const QString &s : rightSubs) {
            if (!seen.contains(s))
                subs.append(s);
        }
        std::sort(subs.begin(), subs.end());
        for (const QString &s : subs)
            pending.append(relDir.isEmpty() ? s : relDir + QLatin1Char('/') + s);
    }

    if (!batch.isEmpty() && !cancelled())
        emit entriesReady(m_scanId, batch);

    emit finished(m_scanId, cancelled());
}
