#pragma once

#include "FramelessDialog.h"

#include "update/UpdateChecker.h" // UpdateInfo

class QLabel;
class QLineEdit;
class QTextEdit;
class QPushButton;

// Announces one available release and points at where to get it. It does NOT
// download or install anything: updates are delivered through the Microsoft
// Store or by the user fetching the package themselves, so the application's
// job here ends at telling them a newer version exists and handing them the
// address and the checksum to verify it with.
class UpdateDialog : public FramelessDialog {
    Q_OBJECT

public:
    explicit UpdateDialog(const UpdateInfo &info, QWidget *parent = nullptr);

    // Test seams: what the dialog is offering the user.
    QString downloadUrlText() const;
    QString checksumText() const;
    bool hasStoreButton() const;

private slots:
    void openDownloadPage();
    void openStorePage();

private:
    UpdateInfo m_info;

    QLabel *m_headlineLabel = nullptr;
    QLabel *m_dateLabel = nullptr;
    QTextEdit *m_notesEdit = nullptr;
    QLineEdit *m_urlField = nullptr;
    QLineEdit *m_checksumField = nullptr;
    QPushButton *m_downloadButton = nullptr;
    QPushButton *m_storeButton = nullptr;
    QPushButton *m_closeButton = nullptr;
};
