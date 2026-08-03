#include "UpdateDialog.h"

#include "update/Updater.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTextEdit>
#include <QVBoxLayout>

#include "version.h"

UpdateDialog::UpdateDialog(const UpdateInfo &info, QWidget *parent)
    : FramelessDialog(parent), m_info(info), m_updater(new Updater(this)) {
    setWindowTitle(tr("Software Update"));
    setModal(true);
    resize(440, 380);

    m_headlineLabel = new QLabel(
        tr("Version %1 is available (you have %2).").arg(info.version, QStringLiteral(TTC_VERSION)),
        this);
    m_headlineLabel->setWordWrap(true);
    QFont headlineFont = m_headlineLabel->font();
    headlineFont.setBold(true);
    m_headlineLabel->setFont(headlineFont);

    m_dateLabel = new QLabel(this);
    if (!info.date.isEmpty())
        m_dateLabel->setText(tr("Released: %1").arg(info.date));

    auto *notesTitle = new QLabel(tr("Release notes:"), this);

    m_notesEdit = new QTextEdit(this);
    m_notesEdit->setReadOnly(true);
    m_notesEdit->setPlainText(info.notes.isEmpty() ? tr("No release notes provided.")
                                                   : info.notes);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);

    m_progressBar = new QProgressBar(this);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->hide(); // shown once the download starts

    m_confirmButton = new QPushButton(tr("Update Now"), this);
    m_confirmButton->setDefault(true);
    m_cancelButton = new QPushButton(tr("Later"), this);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->addStretch();
    buttonRow->addWidget(m_cancelButton);
    buttonRow->addWidget(m_confirmButton);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_headlineLabel);
    layout->addWidget(m_dateLabel);
    layout->addWidget(notesTitle);
    layout->addWidget(m_notesEdit, 1);
    layout->addWidget(m_statusLabel);
    layout->addWidget(m_progressBar);
    layout->addLayout(buttonRow);

    connect(m_confirmButton, &QPushButton::clicked, this, &UpdateDialog::onConfirm);
    connect(m_cancelButton, &QPushButton::clicked, this, &UpdateDialog::onCancel);
    connect(m_updater, &Updater::progress, this, &UpdateDialog::onProgress);
    connect(m_updater, &Updater::finished, this, &UpdateDialog::onFinished);
}

void UpdateDialog::onConfirm() {
    m_confirmButton->setEnabled(false);
    // "Later" becomes "Cancel" for the duration of the download. Leaving the
    // user with no way out of a download that is crawling (or of a mind change)
    // is worse than the small cost of supporting cancellation: the installer
    // has not started yet, so there is nothing to undo.
    m_downloading = true;
    m_cancelButton->setText(tr("Cancel"));
    m_progressBar->show();
    m_progressBar->setValue(0);
    m_statusLabel->setText(tr("Downloading…"));
    m_updater->apply(m_info);
}

void UpdateDialog::onCancel() {
    if (!m_downloading) {
        reject();
        return;
    }
    m_cancelButton->setEnabled(false);
    m_updater->cancel(); // reports back through onFinished
}

void UpdateDialog::onProgress(int percent) {
    m_progressBar->setValue(percent);
    if (percent >= 100)
        m_statusLabel->setText(tr("Verifying and installing…"));
}

void UpdateDialog::onFinished(bool ok, const QString &message) {
    m_statusLabel->setText(message);
    if (ok) {
        // The replacement process is already running; ask the app to quit so the
        // new instance takes over. Buttons stay disabled — nothing left to do.
        emit restartRequested();
        return;
    }
    // Recoverable failure (or a cancellation): let the user retry or dismiss.
    m_downloading = false;
    m_progressBar->hide();
    m_confirmButton->setEnabled(true);
    m_cancelButton->setEnabled(true);
    m_cancelButton->setText(tr("Later"));
    m_confirmButton->setText(tr("Retry"));
}
