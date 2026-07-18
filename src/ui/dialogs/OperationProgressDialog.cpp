#include "OperationProgressDialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QProgressBar>
#include <QVBoxLayout>

OperationProgressDialog::OperationProgressDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("File Operation"));
    setModal(false);
    resize(420, 120);

    m_descriptionLabel = new QLabel(this);
    m_fileLabel = new QLabel(this);
    m_fileLabel->setWordWrap(true);
    m_progressBar = new QProgressBar(this);
    m_progressBar->setRange(0, 0); // indeterminate until we know a total

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &OperationProgressDialog::cancelRequested);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_descriptionLabel);
    layout->addWidget(m_progressBar);
    layout->addWidget(m_fileLabel);
    layout->addWidget(buttons);
}

void OperationProgressDialog::setDescription(const QString &description) {
    m_descriptionLabel->setText(description);
    m_progressBar->setRange(0, 0);
}

void OperationProgressDialog::setProgress(qint64 done, qint64 total, const QString &currentFile) {
    if (total > 0) {
        m_progressBar->setRange(0, static_cast<int>(total));
        m_progressBar->setValue(static_cast<int>(done));
    }
    if (!currentFile.isEmpty())
        m_fileLabel->setText(currentFile);
}
