#pragma once

#include <QDialog>

class QLabel;
class QProgressBar;

// Modeless progress display for the currently running OperationQueue job.
class OperationProgressDialog : public QDialog {
    Q_OBJECT

public:
    explicit OperationProgressDialog(QWidget *parent = nullptr);

public slots:
    void setDescription(const QString &description);
    void setProgress(qint64 done, qint64 total, const QString &currentFile);

signals:
    void cancelRequested();

private:
    QLabel *m_descriptionLabel;
    QLabel *m_fileLabel;
    QProgressBar *m_progressBar;
};
