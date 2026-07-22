#include "SearchDialog.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QHBoxLayout>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include "SearchEngine.h"

SearchDialog::SearchDialog(const QString &initialPath, QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("Search Files"));
    resize(600, 500);

    m_engine = new SearchEngine(this);
    connect(m_engine, &SearchEngine::resultFound, this, &SearchDialog::onResultFound);
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
    connect(m_searchButton, &QPushButton::clicked, this, &SearchDialog::startSearch);
    connect(m_patternEdit, &QLineEdit::returnPressed, this, &SearchDialog::startSearch);

    m_resultsList = new QListWidget(this);
    connect(m_resultsList, &QListWidget::itemActivated, this, &SearchDialog::onResultActivated);

    m_statusLabel = new QLabel(this);

    // Lists every current result in the active file panel (flat, cross-directory
    // "feed to listbox" view) rather than navigating to a single one.
    m_feedButton = new QPushButton(tr("Send to panel"), this);
    m_feedButton->setToolTip(tr("Show all results in the active panel as a flat list"));
    connect(m_feedButton, &QPushButton::clicked, this, &SearchDialog::feedToPanel);

    auto *bottomRow = new QHBoxLayout;
    bottomRow->addWidget(m_statusLabel, 1);
    bottomRow->addWidget(m_feedButton);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(m_searchButton);
    layout->addWidget(m_resultsList, 1);
    layout->addLayout(bottomRow);
}

void SearchDialog::startSearch() {
    if (m_engine->isRunning())
        return;
    m_resultsList->clear();
    m_resultCount = 0;
    m_statusLabel->setText(tr("Searching..."));
    m_searchButton->setEnabled(false);
    m_engine->start(m_pathEdit->text(), m_patternEdit->text(), m_caseSensitiveCheck->isChecked(),
                     m_subdirsCheck->isChecked());
}

void SearchDialog::onResultFound(const QString &path) {
    m_resultsList->addItem(path);
    ++m_resultCount;
}

void SearchDialog::onFinished() {
    m_searchButton->setEnabled(true);
    m_statusLabel->setText(tr("%1 result(s)").arg(m_resultCount));
    if (m_closePending)
        close();
}

void SearchDialog::onResultActivated() {
    QListWidgetItem *item = m_resultsList->currentItem();
    if (item)
        emit navigateRequested(item->text());
}

void SearchDialog::feedToPanel() {
    QStringList paths;
    paths.reserve(m_resultsList->count());
    for (int i = 0; i < m_resultsList->count(); ++i)
        paths.append(m_resultsList->item(i)->text());
    if (!paths.isEmpty())
        emit feedToPanelRequested(paths);
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
