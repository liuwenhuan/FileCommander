#include "TransferProgressDialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

#include "OperationQueue.h"

namespace {

QString humanBytes(qint64 bytes) {
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double size = static_cast<double>(bytes);
    int unit = 0;
    while (size >= 1024.0 && unit < 4) {
        size /= 1024.0;
        ++unit;
    }
    return unit == 0 ? QStringLiteral("%1 B").arg(bytes)
                     : QStringLiteral("%1 %2").arg(size, 0, 'f', 1).arg(units[unit]);
}

QString humanDuration(qint64 seconds) {
    if (seconds < 60)
        return QStringLiteral("%1s").arg(seconds);
    if (seconds < 3600)
        return QStringLiteral("%1m %2s").arg(seconds / 60).arg(seconds % 60);
    return QStringLiteral("%1h %2m").arg(seconds / 3600).arg((seconds % 3600) / 60);
}

} // namespace

TransferProgressDialog::TransferProgressDialog(OperationQueue *queue, QWidget *parent)
    : QDialog(parent), m_queue(queue) {
    setWindowTitle(tr("Transfers"));
    setModal(false);
    resize(460, 180);

    m_descriptionLabel = new QLabel(this);
    m_fileLabel = new QLabel(this);
    m_fileLabel->setWordWrap(true);
    m_bytesLabel = new QLabel(this);
    m_speedLabel = new QLabel(this);
    m_etaLabel = new QLabel(this);
    m_queueLabel = new QLabel(this);
    m_errorLabel = new QLabel(this);
    m_errorLabel->setWordWrap(true);
    m_errorLabel->setStyleSheet(QStringLiteral("color: #c0392b;"));
    m_progressBar = new QProgressBar(this);
    m_progressBar->setRange(0, 0); // indeterminate until we know a total

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_pauseButton = buttons->addButton(tr("Pause"), QDialogButtonBox::ActionRole);
    connect(m_pauseButton, &QPushButton::clicked, this, &TransferProgressDialog::onPauseClicked);
    connect(buttons, &QDialogButtonBox::rejected, this, [this]() {
        if (m_queue)
            m_queue->cancelCurrent();
    });

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_descriptionLabel);
    layout->addWidget(m_progressBar);
    layout->addWidget(m_bytesLabel);
    layout->addWidget(m_speedLabel);
    layout->addWidget(m_etaLabel);
    layout->addWidget(m_fileLabel);
    layout->addWidget(m_queueLabel);
    layout->addWidget(m_errorLabel);
    layout->addWidget(buttons);

    if (m_queue) {
        connect(m_queue, &OperationQueue::started, this, &TransferProgressDialog::onStarted);
        connect(m_queue, &OperationQueue::progress, this, &TransferProgressDialog::onProgress);
        connect(m_queue, &OperationQueue::queueChanged, this,
                &TransferProgressDialog::onQueueChanged);
        connect(m_queue, &OperationQueue::finished, this, &TransferProgressDialog::onFinished);
        connect(m_queue, &OperationQueue::errorOccurred, this,
                &TransferProgressDialog::onErrorOccurred);
    }
}

void TransferProgressDialog::onPauseClicked() {
    m_paused = !m_paused;
    m_pauseButton->setText(m_paused ? tr("Resume") : tr("Pause"));
    if (!m_queue)
        return;
    if (m_paused)
        m_queue->pauseCurrent();
    else
        m_queue->resumeCurrent();
}

void TransferProgressDialog::onStarted(const QString &description) {
    m_descriptionLabel->setText(description);
    m_bytesLabel->clear();
    m_speedLabel->clear();
    m_etaLabel->clear();
    m_fileLabel->clear();
    m_errorLabel->clear();
    m_paused = false;
    m_pauseButton->setText(tr("Pause"));
    m_progressBar->setRange(0, 0);
    m_timer.start(); // reset the clock for throughput/ETA of this job
}

void TransferProgressDialog::onProgress(qint64 doneItems, qint64 totalItems, qint64 doneBytes,
                                        qint64 totalBytes, const QString &currentFile) {
    // Prefer the byte-based bar (smooth for large transfers); fall back to
    // item counts for byte-less operations.
    if (totalBytes > 0) {
        // Scale to KiB so the int range holds multi-GB transfers.
        m_progressBar->setRange(0, static_cast<int>(totalBytes / 1024 + 1));
        m_progressBar->setValue(static_cast<int>(doneBytes / 1024));
    } else if (totalItems > 0) {
        m_progressBar->setRange(0, static_cast<int>(totalItems));
        m_progressBar->setValue(static_cast<int>(doneItems));
    }

    const double elapsedSec = m_timer.isValid() ? m_timer.elapsed() / 1000.0 : 0.0;

    m_bytesLabel->setText(totalBytes > 0
                              ? tr("%1 of %2  (%3 of %4 items)")
                                    .arg(humanBytes(doneBytes), humanBytes(totalBytes))
                                    .arg(doneItems)
                                    .arg(totalItems)
                              : tr("%1 of %2 items").arg(doneItems).arg(totalItems));

    if (totalBytes > 0 && elapsedSec > 0.2 && doneBytes > 0) {
        const double bytesPerSec = doneBytes / elapsedSec;
        m_speedLabel->setText(tr("Speed: %1/s").arg(humanBytes(static_cast<qint64>(bytesPerSec))));
        const qint64 remaining = totalBytes - doneBytes;
        m_etaLabel->setText(remaining > 0 && bytesPerSec > 0
                                ? tr("ETA: %1").arg(humanDuration(
                                      static_cast<qint64>(remaining / bytesPerSec)))
                                : tr("ETA: --"));
    } else {
        m_speedLabel->clear();
        m_etaLabel->clear();
    }

    if (!currentFile.isEmpty())
        m_fileLabel->setText(currentFile);
}

void TransferProgressDialog::onQueueChanged(int pendingCount) {
    m_queueLabel->setText(pendingCount > 0 ? tr("%1 operation(s) queued").arg(pendingCount)
                                           : QString());
}

void TransferProgressDialog::onFinished(bool ok) {
    if (!ok)
        return;
    // A successful job just completed; if nothing else is queued or running,
    // leave the last state visible rather than resetting — the user can close
    // the dialog themselves. Nothing further to do here.
}

void TransferProgressDialog::onErrorOccurred(const QString &message) {
    m_errorLabel->setText(message);
}
