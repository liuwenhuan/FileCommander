#pragma once

#include <QDialog>
#include <QElapsedTimer>

class QLabel;
class QProgressBar;

// Modeless progress display for the currently running OperationQueue job.
// Shows a byte- or item-based bar plus live throughput, elapsed time, and an
// ETA estimate.
class OperationProgressDialog : public QDialog {
    Q_OBJECT

public:
    explicit OperationProgressDialog(QWidget *parent = nullptr);

public slots:
    void setDescription(const QString &description);
    void setProgress(qint64 doneItems, qint64 totalItems, qint64 doneBytes, qint64 totalBytes,
                     const QString &currentFile);

signals:
    void cancelRequested();

private:
    QLabel *m_descriptionLabel;
    QLabel *m_statsLabel;
    QLabel *m_fileLabel;
    QProgressBar *m_progressBar;
    QElapsedTimer m_timer;
};
