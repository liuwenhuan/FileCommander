#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <memory>

#include "DirectorySync.h"
#include "FileInfo.h"

class FileProvider;

// Walks two directory trees in parallel and classifies every file it finds,
// streaming results out as it goes instead of returning one batch at the end.
//
// Traversal is breadth-first *by directory*: for each relative directory it
// lists the left side, then the right side, pairs up that directory's files,
// emits them, and finally queues the union of both sides' subdirectories. That
// ordering is what lets the UI show real differences within milliseconds of
// opening -- and, just as importantly, it means every emitted entry is already
// final: a later part of the walk can never reclassify an entry that has
// already been handed out.
//
// Threading contract, and why it is deliberately strictly serial:
// the scanner is a worker QObject that the owner moveToThread()s onto a private
// thread (the ChecksumDialog/SecureWipeDialog pattern). Both sides are listed
// from that one thread, one call after another -- the two sides are NEVER read
// concurrently. This matters beyond tidiness: libsmbclient cannot be driven
// concurrently in-process at all (see commit 3a9f440, where even two threads
// holding independent contexts aborted within seconds), so a left/right SMB
// comparison must not fan out. The cost is that remote-to-remote comparison
// runs at single-connection speed; that is the correct trade against a crash.
//
// Cancellation is a shared atomic flag polled before every provider call and
// before every batch, so a stopped scan never outlives its dialog. The worst
// case latency is one outstanding list() call, which network providers already
// bound with their own timeout.
class SyncScanner : public QObject {
    Q_OBJECT

public:
    // A batch is flushed once this many entries have accumulated, or once
    // kFlushIntervalMs has passed since the last flush -- whichever comes first.
    //
    // Both limits are needed, and the measurements say why. A flat directory of
    // 1377 files on a real SMB share fills the size limit and streams out in 7
    // batches; the size limit alone is what keeps that from arriving as one lump.
    // But a deep tree is the opposite case: flushing at every directory boundary
    // turned 43200 local entries into 3600 emissions of ~12 rows each, i.e. 3600
    // queued cross-thread signals and 3600 separate row insertions for no visible
    // benefit. Carrying a partial batch across directory boundaries and letting
    // the clock force it out instead keeps the UI equally live (a flush at least
    // every 100 ms) at a fraction of the signal traffic.
    static constexpr int kBatchSize = 200;
    static constexpr int kFlushIntervalMs = 100;

    // Providers may be null, meaning "the local filesystem" (the fast
    // QDirIterator path). They are borrowed: the owner must keep them alive for
    // the scan's lifetime, which SyncDialog does by holding shared_ptrs.
    //
    // `scanId` is echoed back in every signal so an owner that restarts a scan
    // can discard results still in flight from the previous one.
    SyncScanner(QString leftDir, FileProvider *leftProvider, QString rightDir,
                FileProvider *rightProvider, bool recursive,
                std::shared_ptr<std::atomic<bool>> cancel, quint64 scanId = 0);

public slots:
    // Runs the whole walk. Invoked once via a queued connection after the
    // scanner has been moved to its thread.
    void process();

signals:
    // A chunk of freshly classified entries, in traversal order. Each entry is
    // final and will not be superseded.
    void entriesReady(quint64 scanId, const QVector<SyncEntry> &entries);
    // Progress heartbeat: how many entries have been classified so far, and
    // which relative directory is being read right now ("" for the root).
    void progress(quint64 scanId, int scannedCount, const QString &currentDir);
    // The walk ended. `cancelled` distinguishes a user stop (partial results)
    // from a complete run.
    void finished(quint64 scanId, bool cancelled);

private:
    // Lists one side's directory. Returns false if the directory could not be
    // read at all (missing, permission denied, dropped link) -- which is
    // reported as "this side has nothing here" rather than aborting the scan,
    // so one unreadable subtree doesn't kill the whole comparison.
    bool listSide(FileProvider *provider, const QString &baseDir, const QString &relDir,
                  QVector<FileInfo> *files, QStringList *subDirs) const;

    bool cancelled() const { return m_cancel->load(); }

    QString m_leftDir;
    FileProvider *m_leftProvider = nullptr;
    QString m_rightDir;
    FileProvider *m_rightProvider = nullptr;
    bool m_recursive = true;
    std::shared_ptr<std::atomic<bool>> m_cancel;
    quint64 m_scanId = 0;
};
