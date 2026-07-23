#pragma once

#include "FramelessDialog.h"
#include <QObject>
#include <QStringList>

#include <atomic>
#include <memory>

class QTableWidget;
class QProgressBar;
class QPushButton;
class QThread;

// Hashes a batch of files on a worker thread. Each chunk of every file is read
// exactly once and fed to all three digests (MD5, CRC32, SHA1). Progress is
// reported as cumulative bytes across the whole batch so a single QProgressBar
// can track the entire run. The shared atomic<bool> is set by the owning dialog
// when it is closed mid-run; the read loop checks it and bails out cleanly.
class ChecksumWorker : public QObject {
    Q_OBJECT

public:
    ChecksumWorker(QStringList paths, std::shared_ptr<std::atomic<bool>> cancel);

public slots:
    // Runs the whole batch. Invoked once via a queued connection after the
    // worker has been moved to its thread.
    void process();

signals:
    // Emitted (queued) as each file finishes; row is the index into the
    // original path list. On error the digest strings carry a message.
    void rowReady(int row, const QString &md5, const QString &crc32, const QString &sha1);
    // Cumulative bytes hashed so far out of the batch total.
    void progress(qint64 done, qint64 total);
    void finished();

private:
    QStringList m_paths;
    std::shared_ptr<std::atomic<bool>> m_cancel;
};

// Modal-less dialog that computes and displays MD5, CRC32 and SHA1 for a set of
// files. Construct it with the paths to hash and show() it; hashing runs on a
// background thread and the table fills in row by row. Closing the dialog while
// a run is in flight cancels the worker cleanly.
class ChecksumDialog : public FramelessDialog {
    Q_OBJECT

public:
    explicit ChecksumDialog(const QStringList &paths, QWidget *parent = nullptr);
    ~ChecksumDialog() override;

private slots:
    void onRowReady(int row, const QString &md5, const QString &crc32, const QString &sha1);
    void onProgress(qint64 done, qint64 total);
    void onFinished();

private:
    void buildUi();
    void copyAll();
    void stopWorker();

    QStringList m_paths;
    QTableWidget *m_table = nullptr;
    QProgressBar *m_progress = nullptr;
    QPushButton *m_copyButton = nullptr;
    QThread *m_thread = nullptr;
    std::shared_ptr<std::atomic<bool>> m_cancel;
};
