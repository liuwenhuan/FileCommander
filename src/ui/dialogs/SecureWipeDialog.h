#pragma once

#include "FramelessDialog.h"
#include <QObject>
#include <QStringList>

#include <atomic>
#include <memory>

class QTableWidget;
class QProgressBar;
class QThread;

// Securely erases a batch of files/directories on a worker thread: it overwrites
// each file's on-disk bytes before unlinking it, so the contents can't be
// recovered. Linux adapts the pass count to the storage type; Windows uses the
// conservative three-pass policy (0x00, 0xFF, random) because drive-letter
// APIs cannot reliably classify every attached device. Progress is cumulative bytes-written across the whole
// batch (size x passes). The shared atomic<bool> lets the owning dialog cancel a
// run in flight; the loops poll it between chunks.
class WipeWorker : public QObject {
    Q_OBJECT

public:
    WipeWorker(QStringList paths, std::shared_ptr<std::atomic<bool>> cancel);

public slots:
    // Runs the whole batch. Invoked once via a queued connection after the
    // worker has been moved to its thread.
    void process();

signals:
    // Emitted (queued) as each top-level selected path finishes; row is its
    // index in the original list, status a short human message.
    void rowReady(int row, const QString &status);
    // Cumulative bytes overwritten so far out of the batch total.
    void progress(qint64 done, qint64 total);
    void finished();

private:
    QStringList m_paths;
    std::shared_ptr<std::atomic<bool>> m_cancel;
};

// Modal-less dialog that overwrites-then-deletes a set of files. Construct it
// with the paths and show() it (confirmation happens before this, in the
// caller). Wiping runs on a background thread; the table fills in row by row and
// filesChanged() fires when the run completes so the caller can refresh its
// views. Closing the dialog mid-run cancels the worker cleanly.
class SecureWipeDialog : public FramelessDialog {
    Q_OBJECT

public:
    explicit SecureWipeDialog(const QStringList &paths, QWidget *parent = nullptr);
    ~SecureWipeDialog() override;

signals:
    void filesChanged(); // wiping finished; on-disk state changed

private slots:
    void onRowReady(int row, const QString &status);
    void onProgress(qint64 done, qint64 total);
    void onFinished();

private:
    void buildUi();
    void stopWorker();

    QStringList m_paths;
    QTableWidget *m_table = nullptr;
    QProgressBar *m_progress = nullptr;
    QThread *m_thread = nullptr;
    std::shared_ptr<std::atomic<bool>> m_cancel;
};
