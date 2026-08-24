#pragma once

#include "FramelessDialog.h"
#include "update/UpdateChecker.h"

class QLabel;
class QLineEdit;
class QTextEdit;
class QPushButton;

// Announces a release. FileCommander never downloads or installs it.
class UpdateDialog : public FramelessDialog {
    Q_OBJECT

public:
    explicit UpdateDialog(const UpdateInfo &info, QWidget *parent = nullptr);

    QString updatePageText() const;

private slots:
    void openUpdatePage();

private:
    UpdateInfo m_info;
    QLabel *m_headlineLabel = nullptr;
    QLabel *m_dateLabel = nullptr;
    QTextEdit *m_notesEdit = nullptr;
    QLineEdit *m_pageField = nullptr;
    QPushButton *m_openButton = nullptr;
    QPushButton *m_closeButton = nullptr;
};
