#include "OperationProgressDialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

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

OperationProgressDialog::OperationProgressDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("File Operation"));
    setModal(false);
    resize(460, 150);

    m_descriptionLabel = new QLabel(this);
    m_statsLabel = new QLabel(this);
    m_fileLabel = new QLabel(this);
    m_fileLabel->setWordWrap(true);
    m_queueLabel = new QLabel(this);
    m_progressBar = new QProgressBar(this);
    m_progressBar->setRange(0, 0); // indeterminate until we know a total

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_pauseButton = buttons->addButton(tr("Pause"), QDialogButtonBox::ActionRole);
    connect(m_pauseButton, &QPushButton::clicked, this, [this]() {
        m_paused = !m_paused;
        m_pauseButton->setText(m_paused ? tr("Resume") : tr("Pause"));
        if (m_paused)
            emit pauseRequested();
        else
            emit resumeRequested();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &OperationProgressDialog::cancelRequested);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_descriptionLabel);
    layout->addWidget(m_progressBar);
    layout->addWidget(m_statsLabel);
    layout->addWidget(m_fileLabel);
    layout->addWidget(m_queueLabel);
    layout->addWidget(buttons);
}

void OperationProgressDialog::setQueuedCount(int pending) {
    m_queueLabel->setText(pending > 0 ? tr("%1 operation(s) queued").arg(pending) : QString());
}

void OperationProgressDialog::setDescription(const QString &description) {
    m_descriptionLabel->setText(description);
    m_statsLabel->clear();
    m_fileLabel->clear();
    m_paused = false;
    m_pauseButton->setText(tr("Pause"));
    m_progressBar->setRange(0, 0);
    m_timer.start(); // reset the clock for throughput/ETA of this job
}

void OperationProgressDialog::setProgress(qint64 doneItems, qint64 totalItems, qint64 doneBytes,
                                          qint64 totalBytes, const QString &currentFile) {
    // Prefer the byte-based bar (smooth for large files); fall back to item
    // counts for byte-less operations like delete and symlink.
    if (totalBytes > 0) {
        // Scale to KiB so the int range holds multi-GB transfers.
        m_progressBar->setRange(0, static_cast<int>(totalBytes / 1024 + 1));
        m_progressBar->setValue(static_cast<int>(doneBytes / 1024));
    } else if (totalItems > 0) {
        m_progressBar->setRange(0, static_cast<int>(totalItems));
        m_progressBar->setValue(static_cast<int>(doneItems));
    }

    const double elapsedSec = m_timer.isValid() ? m_timer.elapsed() / 1000.0 : 0.0;
    QString stats = tr("%1 of %2 items").arg(doneItems).arg(totalItems);
    if (totalBytes > 0) {
        stats += tr("  ·  %1 / %2").arg(humanBytes(doneBytes), humanBytes(totalBytes));
        if (elapsedSec > 0.2 && doneBytes > 0) {
            const double bytesPerSec = doneBytes / elapsedSec;
            stats += tr("  ·  %1/s").arg(humanBytes(static_cast<qint64>(bytesPerSec)));
            const qint64 remaining = totalBytes - doneBytes;
            if (remaining > 0 && bytesPerSec > 0)
                stats += tr("  ·  ETA %1")
                             .arg(humanDuration(static_cast<qint64>(remaining / bytesPerSec)));
        }
    }
    stats += tr("  ·  elapsed %1").arg(humanDuration(static_cast<qint64>(elapsedSec)));
    m_statsLabel->setText(stats);

    if (!currentFile.isEmpty())
        m_fileLabel->setText(currentFile);
}
