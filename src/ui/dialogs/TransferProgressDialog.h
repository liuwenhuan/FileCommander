#pragma once

#include "FramelessDialog.h"
#include <QElapsedTimer>

class QLabel;
class QProgressBar;
class QPushButton;
class QTimer;
class QGraphicsOpacityEffect;
class QPropertyAnimation;
class QVariantAnimation;
class QShowEvent;
class QHideEvent;
class QEvent;
class QColor;
class OperationQueue;

// Modeless progress display for OperationQueue's concurrent provider
// transfers (SFTP/FTP/WebDAV). Unlike OperationProgressDialog (which a caller
// wires up signal-by-signal), this dialog is self-contained: give it the
// OperationQueue in its constructor and it connects to the queue's existing
// started/progress/finished/queueChanged/errorOccurred signals itself, and
// drives cancelCurrent()/pauseCurrent()/resumeCurrent()/abortAll() directly, so
// a caller only needs to construct it -- it decides when to make itself visible.
//
// Cancel vs Abort (取消 / 中止): Cancel asks the batch to stop and leaves the
// window to run out its normal lifecycle (it stays up when the batch ended with
// an error message, so the message is readable). Abort is the escape hatch -- it
// stops the batch and closes this window in the same gesture, which is also the
// only way out of a window left standing by a reported error.
//
// Visibility policy (so quick operations never flash a window): the dialog stays
// hidden when a job starts and shows itself only if the job is still running
// after a short delay (kShowDelayMs), OR immediately once the reported total is
// obviously large (kBigBytes / kBigItems). It hides again when the queue drains
// (unless an error is showing). This mirrors GNOME Files / Finder's deferred
// progress and the user-chosen "1s delay + immediate for large" policy.
//
// The queue may run several transfers concurrently (see
// OperationQueue::setMaxConcurrentTransfers); this dialog shows the most
// recently reported job's progress (current file, bytes done/total, transfer
// speed, ETA) plus how many operations remain queued, mirroring
// OperationProgressDialog's presentation.
class TransferProgressDialog : public FramelessDialog {
    Q_OBJECT

public:
    explicit TransferProgressDialog(OperationQueue *queue, QWidget *parent = nullptr);
    void dismissAfterAbort();

    // While suppressed, showIfHidden() remembers that it wants to show but does not
    // actually call show()/raise(). MainWindow holds this while a modal
    // OperationErrorDialog is open so the deferred-show timer firing mid-decision (a
    // slow operation, or simply the user taking a moment to read the prompt) can never
    // paint the progress window over the decision the operation is blocked on -- this
    // was observed in practice to beat relying on window-manager z-order alone.
    void suppressAutoShow(bool suppressed);

private slots:
    void onStarted(const QString &description);
    void onProgress(qint64 doneItems, qint64 totalItems, qint64 doneBytes, qint64 totalBytes,
                    const QString &currentFile);
    void onQueueChanged(int pendingCount);
    void onFinished(bool ok);
    void onErrorOccurred(const QString &message);
    void onPauseClicked();
    // 中止: stop the whole batch now and take this window away. See the
    // implementation for what "now" can and cannot promise.
    void onAbortClicked();

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    // Reveal the dialog now (deferred-show timer fired, or a big total arrived).
    void showIfHidden();
    void startRevealAnimation();
    void animateOutcomeColor(const QColor &target);
    void setProgressColor(const QColor &color);
    // Grows the dialog so the wrapping labels (the file path, the error line)
    // are fully visible instead of being clipped by the starting height.
    void fitWrappedText();

    // Visibility policy thresholds.
    static constexpr int kShowDelayMs = 1000;                    // deferred-show delay
    static constexpr int kOutcomeDurationMs = 180;               // terminal-state lifetime
    static constexpr qint64 kBigBytes = 100LL * 1024 * 1024;     // >100 MiB shows at once
    static constexpr qint64 kBigItems = 200;                     // >200 items shows at once

    OperationQueue *m_queue = nullptr;

    QLabel *m_descriptionLabel = nullptr;
    QLabel *m_fileLabel = nullptr;
    QLabel *m_bytesLabel = nullptr;
    QLabel *m_speedLabel = nullptr;
    QLabel *m_etaLabel = nullptr;
    QLabel *m_queueLabel = nullptr;
    QLabel *m_errorLabel = nullptr;
    QProgressBar *m_progressBar = nullptr;
    QPushButton *m_pauseButton = nullptr;
    QPushButton *m_abortButton = nullptr;
    QGraphicsOpacityEffect *m_revealEffect = nullptr;
    QPropertyAnimation *m_revealAnimation = nullptr;
    QVariantAnimation *m_outcomeColorAnimation = nullptr;
    QColor m_defaultProgressColor;

    QElapsedTimer m_timer;
    QTimer *m_showTimer = nullptr;    // single-shot deferred-show timer (kShowDelayMs)
    QTimer *m_terminalHideTimer = nullptr; // owns the shared successful-outcome window
    bool m_paused = false;
    bool m_showSuppressed = false;          // an OperationErrorDialog is currently open
    bool m_wantsShowWhileSuppressed = false; // showIfHidden() was called during suppression
    bool m_shown = false;   // dialog currently visible for the running batch
    bool m_batchActive = false;
    bool m_batchOk = true;
    bool m_hasError = false; // keep the dialog up after finish if an error showed
    int m_activeJobs = 0;
    int m_pendingJobs = 0;
};
