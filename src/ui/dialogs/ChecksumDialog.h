#pragma once

#include "FramelessDialog.h"
#include "FileInfo.h"
#include <QObject>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <memory>

class FileProvider;
class QTableWidget;
class QProgressBar;
class QPushButton;
class QThread;

// Hashes a batch of files on a worker thread. Each chunk of every file is read
// exactly once and fed to all three digests (MD5, CRC32, SHA1). Progress is
// reported as cumulative bytes across the whole batch so a single QProgressBar
// can track the entire run. The shared atomic<bool> is set by the owning dialog
// when it is closed mid-run; the read loop checks it and bails out cleanly.
//
// Two sources of bytes, picked by which constructor was used:
//   * paths -- local files, opened with QFile.
//   * FileInfos + provider -- entries on a share or inside an archive, streamed
//     through the provider's openRead/read. Handing those paths to QFile would
//     hash whatever LOCAL file shares the name and label the result with the
//     remote one, which is worse than the "no files selected" it used to be.
class ChecksumWorker : public QObject {
    Q_OBJECT

public:
    ChecksumWorker(QStringList paths, std::shared_ptr<std::atomic<bool>> cancel);
    // Provider-backed batch. `provider` is held by shared ownership for the run,
    // since the panel that supplied it may be closed or navigated away
    // meanwhile. Sizes for the progress denominator come from the cached
    // listing, so nothing is stat-ed over the wire.
    ChecksumWorker(QVector<FileInfo> infos, std::shared_ptr<FileProvider> provider,
                   std::shared_ptr<std::atomic<bool>> cancel);

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
    // The two halves of process(), one per source of bytes (see class note).
    void processLocal();
    void processProvider();

    QStringList m_paths;
    QVector<FileInfo> m_infos;              // provider-backed runs only
    std::shared_ptr<FileProvider> m_provider; // null -> local QFile run
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
    // Provider-backed entries (network share, archive): the bytes are streamed
    // through `provider` instead of being read off the local filesystem.
    ChecksumDialog(const QVector<FileInfo> &infos, std::shared_ptr<FileProvider> provider,
                   QWidget *parent = nullptr);
    ~ChecksumDialog() override;

private slots:
    void onRowReady(int row, const QString &md5, const QString &crc32, const QString &sha1);
    void onProgress(qint64 done, qint64 total);
    void onFinished();

private:
    void buildUi();
    void copyAll();
    void stopWorker();
    // Moves `worker` onto its own thread, wires its signals up, and starts it.
    // Takes ownership: the thread's finished() deletes it.
    void startWorker(ChecksumWorker *worker);

    QStringList m_paths;
    QTableWidget *m_table = nullptr;
    QProgressBar *m_progress = nullptr;
    QPushButton *m_copyButton = nullptr;
    QThread *m_thread = nullptr;
    std::shared_ptr<std::atomic<bool>> m_cancel;
};
