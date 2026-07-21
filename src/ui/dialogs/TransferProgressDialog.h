#pragma once

#include <QDialog>
#include <QElapsedTimer>

class QLabel;
class QProgressBar;
class QPushButton;
class OperationQueue;

// Modeless progress display for OperationQueue's concurrent provider
// transfers (SFTP/FTP/WebDAV). Unlike OperationProgressDialog (which a caller
// wires up signal-by-signal), this dialog is self-contained: give it the
// OperationQueue in its constructor and it connects to the queue's existing
// started/progress/finished/queueChanged/errorOccurred signals itself, and
// drives cancelCurrent()/pauseCurrent()/resumeCurrent() directly, so a caller
// only needs to construct and show() it.
//
// The queue may run several transfers concurrently (see
// OperationQueue::setMaxConcurrentTransfers); this dialog shows the most
// recently reported job's progress (current file, bytes done/total, transfer
// speed, ETA) plus how many operations remain queued, mirroring
// OperationProgressDialog's presentation.
class TransferProgressDialog : public QDialog {
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
    bool m_paused = false;
};
