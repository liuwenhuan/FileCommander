#pragma once

#include "FramelessDialog.h"
#include <QElapsedTimer>

class QLabel;
class QProgressBar;
class QPushButton;

// Modeless progress display for the currently running OperationQueue job.
// Shows a byte- or item-based bar plus live throughput, elapsed time, and an
// ETA estimate.
class OperationProgressDialog : public FramelessDialog {
    Q_OBJECT

public:
    explicit OperationProgressDialog(QWidget *parent = nullptr);

public slots:
    void setDescription(const QString &description);
    void setProgress(qint64 doneItems, qint64 totalItems, qint64 doneBytes, qint64 totalBytes,
                     const QString &currentFile);
    void setQueuedCount(int pending);

signals:
    void cancelRequested();
    void pauseRequested();
    void resumeRequested();

private:
    QLabel *m_descriptionLabel;
    QLabel *m_statsLabel;
    QLabel *m_fileLabel;
    QLabel *m_queueLabel;
    QProgressBar *m_progressBar;
    QPushButton *m_pauseButton;
    QElapsedTimer m_timer;
    bool m_paused = false;
};
