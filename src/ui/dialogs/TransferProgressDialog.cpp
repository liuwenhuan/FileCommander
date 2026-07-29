#include "TransferProgressDialog.h"
#include "ThemedDialogs.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
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
    : FramelessDialog(parent), m_queue(queue) {
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
    ttc::localizeStandardButtons(buttons);
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

    // Deferred-show timer: a job that finishes within kShowDelayMs never pops a
    // window. Single-shot, re-armed on each started while hidden.
    m_showTimer = new QTimer(this);
    m_showTimer->setSingleShot(true);
    m_showTimer->setInterval(kShowDelayMs);
    connect(m_showTimer, &QTimer::timeout, this, [this] {
        if (m_running)
            showIfHidden();
    });

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

void TransferProgressDialog::showIfHidden() {
    if (m_shown)
        return;
    m_shown = true;
    show();
    raise();
}

void TransferProgressDialog::onStarted(const QString &description) {
    m_descriptionLabel->setText(description);
    m_bytesLabel->clear();
    m_speedLabel->clear();
    m_etaLabel->clear();
    m_fileLabel->clear();
    m_errorLabel->clear();
    m_hasError = false;
    m_paused = false;
    m_pauseButton->setText(tr("Pause"));
    m_progressBar->setRange(0, 0);
    m_timer.start(); // reset the clock for throughput/ETA of this job
    m_running = true;
    // Don't show yet: arm the deferred-show timer so a quick job never flashes a
    // window. onProgress may reveal it sooner if the total turns out to be large.
    if (!m_shown && !m_showTimer->isActive())
        m_showTimer->start();
}

void TransferProgressDialog::onProgress(qint64 doneItems, qint64 totalItems, qint64 doneBytes,
                                        qint64 totalBytes, const QString &currentFile) {
    // Reveal immediately for an obviously large operation, before the 1s delay,
    // so a big copy/move gives instant feedback (the user-chosen policy).
    if (!m_shown && (totalBytes > kBigBytes || totalItems > kBigItems))
        showIfHidden();

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
    // The queue drained. Cancel any pending deferred-show and drop the running
    // flag so a late timer tick can't pop an empty window.
    m_running = false;
    m_showTimer->stop();
    // Auto-hide the dialog once done, unless an error is on display (then the
    // user closes it after reading). A quick job that never showed stays hidden.
    if (m_shown && ok && !m_hasError) {
        m_shown = false;
        hide();
    }
}

void TransferProgressDialog::onErrorOccurred(const QString &message) {
    m_errorLabel->setText(message);
    // Only keep an already-visible dialog up past completion so the message is
    // readable. Don't pop the dialog just for an error: MainWindow already shows
    // a consolidated warning on finish, and a quick failed op shouldn't flash two
    // windows.
    if (m_shown)
        m_hasError = true;
}
