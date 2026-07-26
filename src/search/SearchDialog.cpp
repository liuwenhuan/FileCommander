#include "SearchDialog.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QShowEvent>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QHBoxLayout>
#include <QListView>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include "FileProvider.h"
#include "SearchEngine.h"

namespace {
// Per-item flag recording whether a result is a directory, so activating one
// navigates into it rather than to its parent. Stored on the item because the
// path string alone cannot answer it for a remote hit.
constexpr int kIsDirRole = Qt::UserRole + 1;
} // namespace

SearchDialog::SearchDialog(const QString &initialPath, std::shared_ptr<FileProvider> provider,
                            QWidget *parent)
    : FramelessDialog(parent), m_provider(std::move(provider)) {
    setWindowTitle(tr("Search Files"));
    resize(600, 500);

    m_engine = new SearchEngine(this);
    connect(m_engine, &SearchEngine::resultsFound, this, &SearchDialog::onResultsFound);
    connect(m_engine, &SearchEngine::scanning, this, &SearchDialog::onScanning);
    connect(m_engine, &SearchEngine::finished, this, &SearchDialog::onFinished);

    m_pathEdit = new QLineEdit(initialPath, this);
    m_patternEdit = new QLineEdit(QStringLiteral("*"), this);
    m_caseSensitiveCheck = new QCheckBox(tr("Case sensitive"), this);
    m_subdirsCheck = new QCheckBox(tr("Include subdirectories"), this);
    m_subdirsCheck->setChecked(true);

    auto *form = new QFormLayout;
    form->addRow(tr("Search in:"), m_pathEdit);
    form->addRow(tr("Name pattern:"), m_patternEdit);
    form->addRow(QString(), m_caseSensitiveCheck);
    form->addRow(QString(), m_subdirsCheck);

    m_searchButton = new QPushButton(tr("Search"), this);
    connect(m_searchButton, &QPushButton::clicked, this, &SearchDialog::onSearchButtonClicked);
    connect(m_patternEdit, &QLineEdit::returnPressed, this, &SearchDialog::startSearch);

    m_resultsList = new QListWidget(this);
    // Every row is a single line of the same height; telling the view so lets it
    // skip per-item size negotiation, and batched layout keeps insertion cheap
    // even when a search streams thousands of paths in at once.
    m_resultsList->setUniformItemSizes(true);
    m_resultsList->setLayoutMode(QListView::Batched);
    connect(m_resultsList, &QListWidget::itemActivated, this, &SearchDialog::onResultActivated);

    m_statusLabel = new QLabel(this);

    // Lists every current result in the active file panel (flat, cross-directory
    // "feed to listbox" view) rather than navigating to a single one.
    m_feedButton = new QPushButton(tr("Send to panel"), this);
    m_feedButton->setToolTip(tr("Show all results in the active panel as a flat list"));
    connect(m_feedButton, &QPushButton::clicked, this, &SearchDialog::feedToPanel);
    if (m_provider) {
        // A flat listing is built by re-statting each path on the local
        // filesystem (FileSystemModel::setFlatEntries), which a remote path
        // cannot answer -- the tab would come up empty. Disable the button
        // rather than hand the user a blank tab; single results still open.
        m_feedButton->setEnabled(false);
        m_feedButton->setToolTip(tr("Not available for network locations"));
    }

    auto *bottomRow = new QHBoxLayout;
    bottomRow->addWidget(m_statusLabel, 1);
    bottomRow->addWidget(m_feedButton);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(m_searchButton);
    layout->addWidget(m_resultsList, 1);
    layout->addLayout(bottomRow);
}

void SearchDialog::onSearchButtonClicked() {
    // One button drives both actions: it starts a search when idle and stops the
    // running one when a search is in progress (its label reflects the mode).
    if (m_engine->isRunning()) {
        m_engine->cancel();
        m_statusLabel->setText(tr("Stopping..."));
        m_searchButton->setEnabled(false); // re-enabled by onFinished()
        return;
    }
    startSearch();
}

void SearchDialog::startSearch() {
    if (m_engine->isRunning())
        return;
    m_resultsList->clear();
    m_resultCount = 0;
    m_statusLabel->setText(tr("Searching..."));
    m_searchButton->setText(tr("Stop search")); // becomes a cancel button while running
    m_engine->start(m_pathEdit->text(), m_patternEdit->text(), m_caseSensitiveCheck->isChecked(),
                     m_subdirsCheck->isChecked(), m_provider);
}

void SearchDialog::onResultsFound(const QVector<SearchHit> &hits) {
    for (const SearchHit &hit : hits) {
        auto *item = new QListWidgetItem(hit.path, m_resultsList);
        item->setData(kIsDirRole, hit.isDir);
    }
    m_resultCount += hits.size();
    m_statusLabel->setText(tr("Searching... %1 found").arg(m_resultCount));
}

void SearchDialog::onScanning(const QString &dir) {
    // Only the engine's provider walk emits this, and only every few hundred
    // milliseconds; a network search can spend seconds inside one directory, so
    // without it the status line looks stuck at its last count.
    m_statusLabel->setText(
        tr("Searching %1... %2 found").arg(elideDir(dir)).arg(m_resultCount));
}

QString SearchDialog::elideDir(const QString &dir) const {
    constexpr int kMaxChars = 48;
    if (dir.size() <= kMaxChars)
        return dir;
    // Keep the tail: the deepest components are what change as the walk moves.
    return QStringLiteral("...") + dir.right(kMaxChars);
}

void SearchDialog::onFinished() {
    m_searchButton->setText(tr("Search"));
    m_searchButton->setEnabled(true);
    if (m_engine->wasTruncated())
        m_statusLabel->setText(
            tr("First %1 results (limit reached -- narrow the pattern)").arg(m_resultCount));
    else
        m_statusLabel->setText(tr("%1 result(s)").arg(m_resultCount));
    if (m_closePending)
        close();
}

void SearchDialog::onResultActivated() {
    QListWidgetItem *item = m_resultsList->currentItem();
    if (item)
        emit navigateRequested(item->text(), item->data(kIsDirRole).toBool());
}

void SearchDialog::feedToPanel() {
    QStringList paths;
    paths.reserve(m_resultsList->count());
    for (int i = 0; i < m_resultsList->count(); ++i)
        paths.append(m_resultsList->item(i)->text());
    if (!paths.isEmpty())
        emit feedToPanelRequested(m_patternEdit->text().trimmed(), paths);
}

void SearchDialog::showEvent(QShowEvent *event) {
    QDialog::showEvent(event);
    // The name pattern is what the user almost always edits first; the directory
    // is prefilled from the active panel. Focus + select the pattern so typing
    // replaces the "*" placeholder straight away.
    m_patternEdit->setFocus();
    m_patternEdit->selectAll();
}

void SearchDialog::closeEvent(QCloseEvent *event) {
    if (m_engine->isRunning()) {
        // Defer actual close until the background search notices
        // cancellation and emits finished() -- SearchEngine's worker
        // lambda captures `this`, so the engine (and this dialog) must
        // outlive that lambda's execution.
        m_engine->cancel();
        m_closePending = true;
        m_statusLabel->setText(tr("Cancelling..."));
        event->ignore();
        return;
    }
    event->accept();
}

void SearchDialog::done(int r) {
    // Escape / reject() land here (never in closeEvent), and QDialog::done()'s
    // WA_DeleteOnClose path would delete this dialog -- and its child
    // SearchEngine -- immediately, while the background worker still touches
    // `this`, causing a crash. Mirror closeEvent(): if a search is running,
    // cancel it and defer teardown until finished() fires; don't call the base.
    if (m_engine->isRunning()) {
        m_engine->cancel();
        m_closePending = true;
        m_statusLabel->setText(tr("Cancelling..."));
        return;
    }
    QDialog::done(r);
}
