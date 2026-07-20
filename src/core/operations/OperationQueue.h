#pragma once

#include <QObject>
#include <QQueue>
#include <QString>
#include <QThread>
#include <functional>

#include "FileOpTypes.h"

class FileOperations;

// Runs one filesystem operation at a time on a background thread so the UI
// never blocks. Conflict prompts (overwrite dialogs) are resolved by asking
// a GUI-thread handler and blocking the worker until it answers.
class OperationQueue : public QObject {
    Q_OBJECT

public:
    explicit OperationQueue(QObject *parent = nullptr);
    ~OperationQueue() override;

    // Invoked (on the GUI thread) whenever a copy/move hits an existing
    // destination file; typically wired to OverwriteConfirmDialog::ask.
    void setConflictHandler(ConflictResolver handler) { m_conflictHandler = std::move(handler); }

    void enqueueCopy(const QStringList &sources, const QString &destDir);
    void enqueueMove(const QStringList &sources, const QString &destDir);
    void enqueueDelete(const QStringList &paths, bool toTrash);
    void enqueueMkdir(const QString &parentDir, const QString &name);
    void enqueueRename(const QString &path, const QString &newName);
    void enqueueSymlink(const QStringList &sources, const QString &destDir);

    // Requests cancellation of the running operation and drops any jobs still
    // queued behind it. Safe to call from the GUI thread; the worker stops at
    // the next per-entry boundary.
    void cancelCurrent();

    bool isBusy() const { return m_busy; }

signals:
    void started(const QString &description);
    void progress(qint64 done, qint64 total, const QString &currentFile);
    void errorOccurred(const QString &message);
    void finished(bool ok);

private:
    struct Job {
        QString description;
        std::function<bool(FileOperations &, QString &)> run;
    };

    ErrorAction askConflict(const QString &source, const QString &destination);
    void maybeStartNext();
    void onWorkerJobDone(bool ok);

    QThread m_workerThread;
    FileOperations *m_ops;
    QQueue<Job> m_queue;
    bool m_busy = false;
    ConflictResolver m_conflictHandler;
};
