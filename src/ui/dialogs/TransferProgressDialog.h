#pragma once

#include "FramelessDialog.h"
#include <QElapsedTimer>

class QLabel;
class QProgressBar;
class QPushButton;
class QTimer;
class OperationQueue;

// Modeless progress display for OperationQueue's concurrent provider
// transfers (SFTP/FTP/WebDAV). Unlike OperationProgressDialog (which a caller
// wires up signal-by-signal), this dialog is self-contained: give it the
// OperationQueue in its constructor and it connects to the queue's existing
// started/progress/finished/queueChanged/errorOccurred signals itself, and
// drives cancelCurrent()/pauseCurrent()/resumeCurrent() directly, so a caller
// only needs to construct it -- it decides when to make itself visible.
//
// Visibility policy (so quick operations never flash a window): the dialog stays
// hidden when a job starts and shows itself only if the job is still running
// after a short delay (kShowDelayMs), OR immediately once the reported total is
// obviously large (kBigBytes / kBigItems). It hides again when the queue drains
// (unless an error is showing). This mirrors GNOME Files / Finder's deferred
// progress and the user-chosen "1s delay + immediate for large" policy.
//
// The queue may run several transfers concurrently (see
// OperationQueue::setMaxConcurrentTransfers); this dialog shows the most
// recently reported job's progress (current file, bytes done/total, transfer
// speed, ETA) plus how many operations remain queued, mirroring
// OperationProgressDialog's presentation.
class TransferProgressDialog : public FramelessDialog {
    Q_OBJECT

public:
    explicit TransferProgressDialog(OperationQueue *queue, QWidget *parent = nullptr);

private slots:
    void onStarted(const QString &description);
    void onProgress(qint64 doneItems, qint64 totalItems, qint64 doneBytes, qint64 totalBytes,
                    const QString &currentFile);
    void onQueueChanged(int pendingCount);
    void onFinished(bool ok);
    void onErrorOccurred(const QString &message);
    void onPauseClicked();

private:
    // Reveal the dialog now (deferred-show timer fired, or a big total arrived).
    void showIfHidden();

    // Visibility policy thresholds.
    static constexpr int kShowDelayMs = 1000;                    // deferred-show delay
    static constexpr qint64 kBigBytes = 100LL * 1024 * 1024;     // >100 MiB shows at once
    static constexpr qint64 kBigItems = 200;                     // >200 items shows at once

    OperationQueue *m_queue;

    QLabel *m_descriptionLabel;
    QLabel *m_fileLabel;
    QLabel *m_bytesLabel;
    QLabel *m_speedLabel;
    QLabel *m_etaLabel;
    QLabel *m_queueLabel;
    QLabel *m_errorLabel;
    QProgressBar *m_progressBar;
    QPushButton *m_pauseButton;

    QElapsedTimer m_timer;
    QTimer *m_showTimer;    // single-shot deferred-show timer (kShowDelayMs)
    bool m_paused = false;
    bool m_shown = false;   // dialog currently visible for the running batch
    bool m_running = false; // a job is active (between started and finished)
    bool m_hasError = false; // keep the dialog up after finish if an error showed
};
