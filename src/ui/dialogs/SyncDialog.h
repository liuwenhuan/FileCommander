#pragma once

#include "FramelessDialog.h"

#include <atomic>
#include <memory>

#include <QVector>

#include "DirectorySync.h"

class QTreeView;
class QCheckBox;
class QPushButton;
class QLabel;
class QProgressBar;
class QThread;
class QTimer;
class QWidget;
class OperationQueue;
class FileProvider;
class SyncModel;

// Commands > Synchronize Directories: a two-pane live comparison of the two
// panels' current directories.
//
// The comparison runs on a worker thread (SyncScanner) and streams its results
// in as it goes, so the window appears immediately and fills with real
// differences within milliseconds instead of freezing until a whole tree has
// been walked. Either side may be a network directory: the scanner reads
// through the panels' FileProviders, which this dialog keeps alive with
// shared_ptrs for the scan's duration.
class SyncDialog : public FramelessDialog {
    Q_OBJECT

public:
    // Providers may be null, meaning the local filesystem. They are held for the
    // dialog's lifetime so a scan can never outlive the backend it reads from.
    SyncDialog(const QString &leftDir, std::shared_ptr<FileProvider> leftProvider,
               const QString &rightDir, std::shared_ptr<FileProvider> rightProvider,
               QWidget *parent = nullptr);
    ~SyncDialog() override;

private slots:
    void startScan();
    void abortScan();
    void onEntriesReady(quint64 scanId, const QVector<SyncEntry> &entries);
    void onProgress(quint64 scanId, int scannedCount, const QString &currentDir);
    void onScanFinished(quint64 scanId, bool cancelled);
    void startSync();

private:
    void buildUi();
    void stopWorker();
    void updateSummary();
    // Enables/disables the controls that must not be used mid-scan, and keeps
    // the sync button's label and tooltip in step with why.
    void updateControlStates();

    QString m_leftDir;
    QString m_rightDir;
    std::shared_ptr<FileProvider> m_leftProvider;
    std::shared_ptr<FileProvider> m_rightProvider;

    SyncModel *m_model = nullptr;
    QTreeView *m_view = nullptr;
    QCheckBox *m_recursiveCheck = nullptr;
    QCheckBox *m_showIdenticalCheck = nullptr;
    QLabel *m_summaryLabel = nullptr;
    QLabel *m_scanStatusLabel = nullptr;
    QProgressBar *m_progressBar = nullptr;
    QWidget *m_progressRow = nullptr;
    QPushButton *m_abortButton = nullptr;
    QPushButton *m_rescanButton = nullptr;
    QPushButton *m_syncButton = nullptr;
    QPushButton *m_allToRightButton = nullptr;
    QPushButton *m_allToLeftButton = nullptr;
    QPushButton *m_allSkipButton = nullptr;

    QThread *m_thread = nullptr;
    std::shared_ptr<std::atomic<bool>> m_cancel;
    bool m_scanning = false;
    // Identifies the current scan. A cancelled scanner keeps running until its
    // outstanding directory listing returns, and its already-queued signals stay
    // in the event queue -- so results from a superseded run must be discarded by
    // id rather than assumed to have stopped arriving.
    quint64 m_scanId = 0;

    OperationQueue *m_queue = nullptr;
    // Coalesces the per-job finished() burst at the end of a sync into a single
    // rescan.
    QTimer *m_rescanDebounce = nullptr;
};
