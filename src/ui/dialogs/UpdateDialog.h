#pragma once

#include "FramelessDialog.h"

#include "update/UpdateChecker.h" // UpdateInfo

class Updater;
class QLabel;
class QTextEdit;
class QProgressBar;
class QPushButton;

// Presents one available release (version, date, notes) and, on confirmation,
// drives an owned Updater through download -> verify -> install, showing a
// progress bar. Self-contained: the caller only shows the dialog and connects
// restartRequested() to quit the current instance (the replacement process is
// already launched by then).
class UpdateDialog : public FramelessDialog {
    Q_OBJECT

public:
    explicit UpdateDialog(const UpdateInfo &info, QWidget *parent = nullptr);

signals:
    // Emitted after a successful update once the replacement process is spawned.
    // The application should quit in response.
    void restartRequested();

private slots:
    void onConfirm();
    // "Later" before the download starts, "Cancel" during it.
    void onCancel();
    void onProgress(int percent);
    void onFinished(bool ok, const QString &message);

private:
    UpdateInfo m_info;
    Updater *m_updater;
    bool m_downloading = false;

    QLabel *m_headlineLabel;
    QLabel *m_dateLabel;
    QTextEdit *m_notesEdit;
    QLabel *m_statusLabel;
    QProgressBar *m_progressBar;
    QPushButton *m_confirmButton;
    QPushButton *m_cancelButton;
};
