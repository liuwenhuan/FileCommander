#include "TransferProgressDialog.h"
#include "ThemedDialogs.h"

#include <QDialogButtonBox>
#include <QEvent>
#include <QGraphicsOpacityEffect>
#include <QHideEvent>
#include <QLabel>
#include <QLayout>
#include <QPalette>
#include <QPropertyAnimation>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QVariantAnimation>
#include <QVBoxLayout>

#include "MotionPolicy.h"
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
    m_bytesLabel = new QLabel(this);
    m_speedLabel = new QLabel(this);
    m_etaLabel = new QLabel(this);
    m_queueLabel = new QLabel(this);
    m_errorLabel = new QLabel(this);
    m_errorLabel->setProperty("semanticState", QStringLiteral("error"));
    // The three lines whose text has no length anybody controls: the job
    // description names a destination directory, the file label names a file,
    // and an error message quotes both. Each would otherwise set this window's
    // minimum width to the longest of them and leave it there -- see
    // OperationProgressDialog for the same measurement. Bytes/speed/ETA/queue
    // are numbers and stay single-line.
    ttc::relaxLabelWidth(m_descriptionLabel);
    ttc::relaxLabelWidth(m_fileLabel);
    ttc::relaxLabelWidth(m_errorLabel);
    m_progressBar = new QProgressBar(this);
    m_progressBar->setRange(0, 0); // indeterminate until we know a total
    m_defaultProgressColor = m_progressBar->palette().color(QPalette::Highlight);
    m_revealEffect = new QGraphicsOpacityEffect(this);
    setGraphicsEffect(m_revealEffect);
    m_revealAnimation = new QPropertyAnimation(m_revealEffect, "opacity", this);
    m_revealAnimation->setObjectName(QStringLiteral("TransferProgressRevealAnimation"));
    m_outcomeColorAnimation = new QVariantAnimation(this);
    m_outcomeColorAnimation->setObjectName(QStringLiteral("TransferProgressOutcomeColorAnimation"));
    connect(m_outcomeColorAnimation, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &value) { setProgressColor(value.value<QColor>()); });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_pauseButton = buttons->addButton(tr("Pause"), QDialogButtonBox::ActionRole);
    m_pauseButton->setObjectName(QStringLiteral("TransferPauseButton"));
    // DestructiveRole, not ActionRole: this is the button that throws work away.
    m_abortButton = buttons->addButton(tr("Abort"), QDialogButtonBox::DestructiveRole);
    m_abortButton->setObjectName(QStringLiteral("TransferAbortButton"));
    ttc::localizeStandardButtons(buttons);
    connect(m_pauseButton, &QPushButton::clicked, this, &TransferProgressDialog::onPauseClicked);
    connect(m_abortButton, &QPushButton::clicked, this, &TransferProgressDialog::onAbortClicked);
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
        if (m_batchActive)
            showIfHidden();
    });

    m_terminalHideTimer = new QTimer(this);
    m_terminalHideTimer->setObjectName(
        QStringLiteral("TransferProgressTerminalHideTimer"));
    m_terminalHideTimer->setSingleShot(true);
    m_terminalHideTimer->setInterval(kOutcomeDurationMs);
    connect(m_terminalHideTimer, &QTimer::timeout, this, [this] {
        if (!m_batchActive && !m_hasError && m_shown) {
            m_shown = false;
            hide();
        }
    });

    if (m_queue) {
        connect(m_queue, &OperationQueue::started, this, &TransferProgressDialog::onStarted);
        connect(m_queue, &OperationQueue::progress, this, &TransferProgressDialog::onProgress);
        connect(m_queue, &OperationQueue::queueChanged, this,
                &TransferProgressDialog::onQueueChanged);
        connect(m_queue, &OperationQueue::finished, this, &TransferProgressDialog::onFinished);
        connect(m_queue, &OperationQueue::errorOccurred, this,
                &TransferProgressDialog::onErrorOccurred);
        // Any abort takes this window down, wherever it came from -- this
        // button, or the Abort choice in the modal OperationErrorDialog.
        connect(m_queue, &OperationQueue::aborted, this,
                &TransferProgressDialog::dismissAfterAbort);
    }
}

void TransferProgressDialog::showEvent(QShowEvent *event) {
    FramelessDialog::showEvent(event);
    fitWrappedText();
    startRevealAnimation();
}

void TransferProgressDialog::hideEvent(QHideEvent *event) {
    FramelessDialog::hideEvent(event);
    // m_shown is "this window is currently on screen for the running batch", and
    // it gates every path that would show it again. Closing the window by its
    // own title-bar button (or Escape) goes nowhere near dismissAfterAbort(), so
    // without this the flag stayed true against an invisible window and the
    // progress window never appeared again for the rest of the session.
    m_shown = false;
}

void TransferProgressDialog::dismissAfterAbort() {
    m_showTimer->stop();
    m_terminalHideTimer->stop();
    m_revealAnimation->stop();
    m_outcomeColorAnimation->stop();
    m_activeJobs = 0;
    m_pendingJobs = 0;
    m_batchActive = false;
    m_batchOk = false;
    m_hasError = false;
    m_paused = false;
    m_shown = false;
    m_queueLabel->clear();
    m_errorLabel->clear();
    hide();
}

void TransferProgressDialog::changeEvent(QEvent *event) {
    FramelessDialog::changeEvent(event);
    if (!m_errorLabel)
        return;
    if (event->type() == QEvent::FontChange || event->type() == QEvent::ApplicationFontChange ||
        event->type() == QEvent::StyleChange) {
        QTimer::singleShot(0, this, [this] { fitWrappedText(); });
    }
}

void TransferProgressDialog::fitWrappedText() {
    // Both wrapping labels, not just the error one. The file path wraps too,
    // and a deep source path (the one that prompted this ran to two lines) was
    // simply cut off by the dialog's starting height -- the text was there, the
    // room for it was not.
    const QMargins layoutMargins = layout() ? layout()->contentsMargins() : QMargins();
    const int textWidth = qMax(1, contentsRect().width() - layoutMargins.left() -
                                     layoutMargins.right());
    for (QLabel *label : {m_fileLabel, m_errorLabel}) {
        if (!label)
            continue;
        label->setMinimumHeight(label->text().isEmpty() ? 0
                                                        : label->heightForWidth(textWidth));
        label->updateGeometry();
    }
    if (layout()) {
        layout()->invalidate();
        layout()->activate();
    }
    const int requiredHeight = minimumSizeHint().height();
    if (height() < requiredHeight)
        resize(width(), requiredHeight);
}

void TransferProgressDialog::startRevealAnimation() {
    m_revealAnimation->stop();
    if (MotionPolicy::reduced()) {
        m_revealEffect->setOpacity(1.0);
        return;
    }

    m_revealEffect->setOpacity(0.0);
    m_revealAnimation->setDuration(120);
    m_revealAnimation->setEasingCurve(MotionPolicy::easing());
    m_revealAnimation->setStartValue(0.0);
    m_revealAnimation->setEndValue(1.0);
    m_revealAnimation->start();
}

void TransferProgressDialog::setProgressColor(const QColor &color) {
    QPalette palette = m_progressBar->palette();
    palette.setColor(QPalette::Highlight, color);
    m_progressBar->setPalette(palette);
}

void TransferProgressDialog::animateOutcomeColor(const QColor &target) {
    m_outcomeColorAnimation->stop();
    const QColor start = m_progressBar->palette().color(QPalette::Highlight);
    if (MotionPolicy::reduced()) {
        setProgressColor(target);
        return;
    }

    m_outcomeColorAnimation->setDuration(kOutcomeDurationMs);
    m_outcomeColorAnimation->setEasingCurve(MotionPolicy::easing());
    m_outcomeColorAnimation->setStartValue(start);
    m_outcomeColorAnimation->setEndValue(target);
    m_outcomeColorAnimation->start();
}

void TransferProgressDialog::onAbortClicked() {
    // What "stops now" means, precisely, because the user is entitled to know
    // what they are left holding:
    //
    //  * Nothing queued but not yet started ever runs. Guaranteed.
    //  * Every worker is told to give up at its next checkpoint, and the
    //    checkpoints are close together -- between entries, between streamed
    //    network chunks, and (since this change) inside a single local file
    //    copy, which used to run to completion no matter what was asked of it.
    //    A worker parked in a blocking syscall the OS won't interrupt (a stalled
    //    network read, a hung mount) still has to come back before it can obey;
    //    "now" is bounded by that, not by the size of the transfer.
    //  * The one file that was mid-write is deleted -- Win32 discards the
    //    destination when the copy routine cancels, and the POSIX loop removes
    //    what it wrote. A directory the batch was filling in is removed too when
    //    the batch created it. So the destination does not keep a truncated file
    //    that looks complete.
    //  * Entries the batch had already finished stay finished. An aborted *move*
    //    is therefore genuinely half-done: what already moved is at the
    //    destination and gone from the source. Nothing is rolled back.
    //
    // The window closes immediately rather than waiting for the workers to
    // unwind; they finish reporting into a dialog that is already gone.
    if (m_queue)
        m_queue->abortAll(); // emits aborted() -> dismissAfterAbort()
    dismissAfterAbort();     // also correct with no queue attached
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
    if (m_showSuppressed) {
        m_wantsShowWhileSuppressed = true;
        return;
    }
    m_shown = true;
    show();
    raise();
}

void TransferProgressDialog::suppressAutoShow(bool suppressed) {
    m_showSuppressed = suppressed;
    if (!suppressed && m_wantsShowWhileSuppressed) {
        m_wantsShowWhileSuppressed = false;
        // Not showIfHidden(): the answer the user just gave may end the batch
        // outright (Skip/Cancel on the only conflicting file), and revealing the
        // window at that instant only to hide it again is exactly the flash the
        // deferred-show policy exists to prevent. Re-arm the delay instead, so
        // the window appears only if there is still work happening a moment later.
        if (m_batchActive && !m_showTimer->isActive())
            m_showTimer->start();
    }
}

void TransferProgressDialog::onStarted(const QString &description) {
    m_descriptionLabel->setText(description);
    m_bytesLabel->clear();
    m_speedLabel->clear();
    m_etaLabel->clear();
    m_fileLabel->clear();
    m_progressBar->setRange(0, 0);
    if (!m_batchActive) {
        m_terminalHideTimer->stop();
        m_errorLabel->clear();
        m_errorLabel->setMinimumHeight(0);
        m_hasError = false;
        m_batchOk = true;
        m_paused = false;
        m_pauseButton->setText(tr("Pause"));
        m_outcomeColorAnimation->stop();
        setProgressColor(m_defaultProgressColor);
        m_batchActive = true;
    }
    ++m_activeJobs;
    m_timer.start(); // reset the clock for throughput/ETA of this job
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

    if (!currentFile.isEmpty() && m_fileLabel->text() != currentFile) {
        m_fileLabel->setText(currentFile);
        // Refit: this is the label that actually changes during a transfer, and
        // each new path wraps to its own number of lines. Fitting only on show
        // sized the dialog for whatever path happened to be first.
        fitWrappedText();
    }
}

void TransferProgressDialog::onQueueChanged(int pendingCount) {
    m_pendingJobs = pendingCount;
    m_queueLabel->setText(pendingCount > 0 ? tr("%1 operation(s) queued").arg(pendingCount)
                                           : QString());
}

void TransferProgressDialog::onFinished(bool ok) {
    if (m_activeJobs > 0)
        --m_activeJobs;
    m_batchOk = m_batchOk && ok;

    // OperationQueue emits finished once per worker job. Keep the batch active
    // until no worker is in flight and no queued job remains.
    if (m_activeJobs > 0 || m_pendingJobs > 0)
        return;

    m_batchActive = false;
    m_showTimer->stop();
    animateOutcomeColor(m_batchOk ? QColor(0x2c, 0xa0, 0x44)
                                  : QColor(0xe0, 0x4a, 0x4a));
    // Auto-hide the dialog once done, unless an error is on display (then the
    // user closes it after reading). A quick job that never showed stays hidden.
    //
    // The gate is the error *message*, not the ok flag: Skip and Cancel in the
    // overwrite prompt both report the batch as failed without emitting an
    // error, so keying on ok left this window on screen with a red bar, an
    // empty message and no way to know it was finished.
    if (m_shown && !m_hasError)
        m_terminalHideTimer->start();
}

void TransferProgressDialog::onErrorOccurred(const QString &message) {
    m_errorLabel->setText(message);
    fitWrappedText();
    animateOutcomeColor(QColor(0xe0, 0x4a, 0x4a));
    // Only keep an already-visible dialog up past completion so the message is
    // readable. Don't pop the dialog just for an error: MainWindow already shows
    // a consolidated warning on finish, and a quick failed op shouldn't flash two
    // windows.
    if (m_shown)
        m_hasError = true;
}
