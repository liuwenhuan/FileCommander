#include "SyncDialog.h"

#include <QCheckBox>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QThread>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>

#include "FileProvider.h"
#include "LocalFileProvider.h"
#include "OperationQueue.h"
#include "OverwriteConfirmDialog.h"
#include "SyncActionDelegate.h"
#include "SyncModel.h"
#include "SyncScanner.h"
#include "ThemedDialogs.h"

namespace {

QString humanSize(qint64 bytes) {
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double size = static_cast<double>(bytes);
    int unit = 0;
    while (size >= 1024.0 && unit < 4) {
        size /= 1024.0;
        ++unit;
    }
    if (unit == 0)
        return QStringLiteral("%1 B").arg(bytes);
    return QStringLiteral("%1 %2").arg(size, 0, 'f', 1).arg(units[unit]);
}

} // namespace

SyncDialog::SyncDialog(const QString &leftDir, std::shared_ptr<FileProvider> leftProvider,
                        const QString &rightDir, std::shared_ptr<FileProvider> rightProvider,
                        QWidget *parent)
    : FramelessDialog(parent), m_leftDir(leftDir), m_rightDir(rightDir),
      m_leftProvider(std::move(leftProvider)), m_rightProvider(std::move(rightProvider)),
      m_cancel(std::make_shared<std::atomic<bool>>(false)) {
    buildUi();

    m_queue = new OperationQueue(this);
    m_queue->setConflictHandler(
        [this](const FileConflict &conflict) { return OverwriteConfirmDialog::ask(this, conflict); });
    connect(m_queue, &OperationQueue::errorOccurred, this, [this](const QString &msg) {
        ttc::warning(this, tr("Synchronize"), msg);
    });
    // A finished sync leaves the two trees in a new state, so the comparison on
    // screen is stale: rescan rather than show numbers that no longer hold. The
    // queue reports each job separately, so coalesce the burst into one rescan
    // that runs after the last of them has landed.
    m_rescanDebounce = new QTimer(this);
    m_rescanDebounce->setSingleShot(true);
    m_rescanDebounce->setInterval(400);
    connect(m_rescanDebounce, &QTimer::timeout, this, &SyncDialog::startScan);
    connect(m_queue, &OperationQueue::finished, this,
            [this](bool) { m_rescanDebounce->start(); });

    // Start the scan only after the window has been shown, so the user sees the
    // dialog (and its "comparing" state) immediately instead of waiting on a
    // first directory listing -- the whole point of the rework.
    QTimer::singleShot(0, this, &SyncDialog::startScan);
}

SyncDialog::~SyncDialog() {
    stopWorker();
}

void SyncDialog::buildUi() {
    setWindowTitle(tr("Synchronize Directories"));
    resize(1040, 640);

    auto *layout = new QVBoxLayout(this);

    // --- Header: which two directories are being compared.
    auto *headerRow = new QHBoxLayout;
    auto *leftPath = new QLabel(tr("Left: %1").arg(m_leftDir), this);
    leftPath->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *rightPath = new QLabel(tr("Right: %1").arg(m_rightDir), this);
    rightPath->setTextInteractionFlags(Qt::TextSelectableByMouse);
    rightPath->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    headerRow->addWidget(leftPath, 1);
    headerRow->addWidget(rightPath, 1);
    layout->addLayout(headerRow);

    // --- Options and bulk actions.
    m_recursiveCheck = new QCheckBox(tr("Include subdirectories"), this);
    m_recursiveCheck->setChecked(true);
    connect(m_recursiveCheck, &QCheckBox::toggled, this, &SyncDialog::startScan);

    m_showIdenticalCheck = new QCheckBox(tr("Show identical files"), this);
    m_showIdenticalCheck->setChecked(false);
    connect(m_showIdenticalCheck, &QCheckBox::toggled, this, [this](bool on) {
        m_model->setShowIdentical(on);
    });

    m_allToRightButton = new QPushButton(tr("All →"), this);
    m_allToRightButton->setToolTip(tr("Set every difference to copy left → right"));
    m_allToLeftButton = new QPushButton(tr("All ←"), this);
    m_allToLeftButton->setToolTip(tr("Set every difference to copy right → left"));
    m_allSkipButton = new QPushButton(tr("Skip all"), this);
    m_allSkipButton->setToolTip(tr("Exclude every row from the sync"));
    connect(m_allToRightButton, &QPushButton::clicked, this,
            [this] { m_model->setAllDirections(SyncModel::Direction::ToRight); });
    connect(m_allToLeftButton, &QPushButton::clicked, this,
            [this] { m_model->setAllDirections(SyncModel::Direction::ToLeft); });
    connect(m_allSkipButton, &QPushButton::clicked, this,
            [this] { m_model->setAllDirections(SyncModel::Direction::Skip); });

    auto *optionsRow = new QHBoxLayout;
    optionsRow->addWidget(m_recursiveCheck);
    optionsRow->addWidget(m_showIdenticalCheck);
    optionsRow->addStretch(1);
    optionsRow->addWidget(m_allToRightButton);
    optionsRow->addWidget(m_allToLeftButton);
    optionsRow->addWidget(m_allSkipButton);
    layout->addLayout(optionsRow);

    // --- Scan progress strip. Shown only while a comparison is running; the
    // abort button lives here, next to the status it cancels, matching how the
    // network search surfaces its own in-progress state.
    m_progressRow = new QWidget(this);
    auto *progressLayout = new QHBoxLayout(m_progressRow);
    progressLayout->setContentsMargins(0, 0, 0, 0);
    m_progressBar = new QProgressBar(m_progressRow);
    // Indeterminate on purpose: a real percentage would need a full pre-scan
    // pass, which on a network share means paying for every directory listing
    // twice. The scanned count and current directory below give honest,
    // continuously-moving feedback instead.
    m_progressBar->setRange(0, 0);
    m_progressBar->setMaximumWidth(160);
    m_progressBar->setTextVisible(false);
    m_scanStatusLabel = new QLabel(m_progressRow);
    m_abortButton = new QPushButton(tr("Abort"), m_progressRow);
    connect(m_abortButton, &QPushButton::clicked, this, &SyncDialog::abortScan);
    progressLayout->addWidget(m_progressBar);
    progressLayout->addWidget(m_scanStatusLabel, 1);
    progressLayout->addWidget(m_abortButton);
    m_progressRow->hide();
    layout->addWidget(m_progressRow);

    // --- The two-pane comparison itself.
    m_model = new SyncModel(this);
    connect(m_model, &SyncModel::summaryChanged, this, &SyncDialog::updateSummary);

    m_view = new QTreeView(this);
    m_view->setModel(m_model);
    m_view->setRootIsDecorated(false);
    m_view->setUniformRowHeights(true); // keeps scrolling cheap on huge trees
    m_view->setAlternatingRowColors(true);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setItemDelegateForColumn(SyncModel::ActionColumn, new SyncActionDelegate(m_model, this));

    QHeaderView *header = m_view->header();
    header->setSectionResizeMode(SyncModel::LeftNameColumn, QHeaderView::Stretch);
    header->setSectionResizeMode(SyncModel::LeftSizeColumn, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(SyncModel::LeftTimeColumn, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(SyncModel::ActionColumn, QHeaderView::Fixed);
    header->setSectionResizeMode(SyncModel::RightNameColumn, QHeaderView::Stretch);
    header->setSectionResizeMode(SyncModel::RightSizeColumn, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(SyncModel::RightTimeColumn, QHeaderView::ResizeToContents);
    m_view->setColumnWidth(SyncModel::ActionColumn, 48);

    // Double-clicking anywhere on a row is a second way to flip its direction,
    // for users who don't discover the clickable action cell.
    connect(m_view, &QTreeView::doubleClicked, this,
            [this](const QModelIndex &index) { m_model->cycleDirection(index.row()); });

    layout->addWidget(m_view, 1);

    // --- Summary and the primary actions.
    m_summaryLabel = new QLabel(this);
    layout->addWidget(m_summaryLabel);

    m_rescanButton = new QPushButton(tr("Compare Again"), this);
    connect(m_rescanButton, &QPushButton::clicked, this, &SyncDialog::startScan);

    m_syncButton = new QPushButton(this);
    m_syncButton->setDefault(true);
    connect(m_syncButton, &QPushButton::clicked, this, &SyncDialog::startSync);

    auto *closeButton = new QPushButton(tr("Close"), this);
    connect(closeButton, &QPushButton::clicked, this, &SyncDialog::reject);

    auto *actionsRow = new QHBoxLayout;
    actionsRow->addWidget(m_rescanButton);
    actionsRow->addStretch(1);
    actionsRow->addWidget(m_syncButton);
    actionsRow->addWidget(closeButton);
    layout->addLayout(actionsRow);

    updateSummary();
    updateControlStates();
}

void SyncDialog::startScan() {
    stopWorker();
    m_model->clearRows();

    m_cancel = std::make_shared<std::atomic<bool>>(false);
    ++m_scanId;
    m_scanning = true;
    m_progressBar->show();
    m_abortButton->show();
    m_abortButton->setEnabled(true);
    m_progressRow->show();
    m_scanStatusLabel->setText(tr("Comparing…"));
    updateControlStates();

    // Same worker shape as ChecksumDialog/SecureWipeDialog: the scanner owns
    // nothing the UI touches and talks back only through queued signals.
    m_thread = new QThread(this);
    auto *scanner = new SyncScanner(m_leftDir, m_leftProvider.get(), m_rightDir,
                                     m_rightProvider.get(), m_recursiveCheck->isChecked(), m_cancel,
                                     m_scanId);
    scanner->moveToThread(m_thread);

    connect(m_thread, &QThread::started, scanner, &SyncScanner::process);
    connect(scanner, &SyncScanner::entriesReady, this, &SyncDialog::onEntriesReady);
    connect(scanner, &SyncScanner::progress, this, &SyncDialog::onProgress);
    connect(scanner, &SyncScanner::finished, this, &SyncDialog::onScanFinished);
    connect(m_thread, &QThread::finished, scanner, &QObject::deleteLater);

    m_thread->start();
}

void SyncDialog::abortScan() {
    if (!m_scanning)
        return;
    m_cancel->store(true);
    m_scanStatusLabel->setText(tr("Stopping…"));
    m_abortButton->setEnabled(false);
}

void SyncDialog::stopWorker() {
    if (!m_thread)
        return;
    // Ask the walk to bail, then let the thread's event loop drain and join, so
    // no scan outlives the dialog. The wait is bounded by one outstanding
    // directory listing, which network providers already time out.
    m_cancel->store(true);
    m_thread->quit();
    m_thread->wait();
    delete m_thread;
    m_thread = nullptr;
    m_scanning = false;
}

void SyncDialog::onEntriesReady(quint64 scanId, const QVector<SyncEntry> &entries) {
    // Late results from a scan that has been restarted or abandoned would
    // otherwise be appended on top of the new run's rows, duplicating them.
    if (scanId != m_scanId)
        return;
    m_model->appendEntries(entries);
}

void SyncDialog::onProgress(quint64 scanId, int scannedCount, const QString &currentDir) {
    if (scanId != m_scanId || !m_scanning)
        return;
    if (currentDir.isEmpty()) {
        m_scanStatusLabel->setText(tr("Comparing… %n item(s) scanned", nullptr, scannedCount));
    } else {
        m_scanStatusLabel->setText(tr("Comparing… %n item(s) scanned — %1", nullptr, scannedCount)
                                        .arg(currentDir));
    }
}

void SyncDialog::onScanFinished(quint64 scanId, bool cancelled) {
    if (scanId != m_scanId)
        return; // a superseded run winding down; the current one is still going
    m_scanning = false;
    if (m_thread) {
        m_thread->quit();
        m_thread->wait();
        delete m_thread;
        m_thread = nullptr;
    }

    if (cancelled) {
        // Keep the strip visible to say plainly that what's on screen is partial.
        m_progressBar->hide();
        m_abortButton->hide();
        m_scanStatusLabel->setText(tr("Comparison stopped — results are incomplete."));
    } else {
        m_progressRow->hide();
    }
    updateControlStates();
    updateSummary();
}

void SyncDialog::updateSummary() {
    const SyncModel::Summary s = m_model->summary();
    const int pending = s.toRight + s.toLeft;
    m_summaryLabel->setText(tr("→ %1 · ← %2 · conflicts %3 · identical %4 · skipped %5    "
                                "To sync: %6 file(s), %7")
                                 .arg(s.toRight)
                                 .arg(s.toLeft)
                                 .arg(s.conflicts)
                                 .arg(s.identical)
                                 .arg(s.skipped)
                                 .arg(pending)
                                 .arg(humanSize(s.bytesToCopy)));
    updateControlStates();
}

void SyncDialog::updateControlStates() {
    if (!m_syncButton)
        return;

    const int pending = m_model->summary().toRight + m_model->summary().toLeft;

    if (m_scanning) {
        m_syncButton->setEnabled(false);
        m_syncButton->setText(tr("Start Sync (comparing…)"));
        // An unexplained disabled button is the most confusing thing a dialog can
        // do, so say why and how to proceed.
        m_syncButton->setToolTip(tr("Available once the comparison finishes. To start now, "
                                     "press Abort first.\n"
                                     "Copying while the scan is still running would make it "
                                     "re-read the files it just wrote and report them as new "
                                     "differences."));
    } else {
        m_syncButton->setEnabled(pending > 0);
        m_syncButton->setText(pending > 0 ? tr("Start Sync (%1)").arg(pending) : tr("Start Sync"));
        m_syncButton->setToolTip(pending > 0
                                      ? tr("Copy the %n selected file(s).", nullptr, pending)
                                      : tr("Nothing to copy: no row is set to a direction."));
    }

    m_rescanButton->setEnabled(!m_scanning);
    m_recursiveCheck->setEnabled(!m_scanning);
}

void SyncDialog::startSync() {
    const QVector<SyncModel::Row> rows = m_model->actionableRows();
    if (rows.isEmpty())
        return;

    LocalFileProvider *local = LocalFileProvider::instance();
    const std::shared_ptr<FileProvider> localProvider(local, [](FileProvider *) {});
    const std::shared_ptr<FileProvider> leftProv =
        m_leftProvider ? m_leftProvider : localProvider;
    const std::shared_ptr<FileProvider> rightProv =
        m_rightProvider ? m_rightProvider : localProvider;

    for (const SyncModel::Row &row : rows) {
        const bool toRight = row.direction == SyncModel::Direction::ToRight;
        const QString &srcBase = toRight ? m_leftDir : m_rightDir;
        const QString &dstBase = toRight ? m_rightDir : m_leftDir;
        const std::shared_ptr<FileProvider> srcProv = toRight ? leftProv : rightProv;
        const std::shared_ptr<FileProvider> dstProv = toRight ? rightProv : leftProv;

        const QString rel = row.entry.relativePath;
        const QString srcPath = srcBase + QLatin1Char('/') + rel;
        const QString relDir = QFileInfo(rel).path();
        // QFileInfo::path() yields "." for a file at the tree root.
        const QString destDir = (relDir == QLatin1String("."))
                                     ? dstBase
                                     : dstBase + QLatin1Char('/') + relDir;

        // A remote endpoint has to stream through the provider engine; treating
        // its path as a local file is what makes network transfers fail.
        if (srcProv.get() != local || dstProv.get() != local) {
            if (dstProv.get() != local)
                m_queue->enqueueProviderMkdir(dstProv, dstBase, relDir);
            m_queue->enqueueProviderCopy(srcProv, {srcPath}, dstProv, destDir);
        } else {
            m_queue->enqueueCopy({srcPath}, destDir);
        }
    }
}
