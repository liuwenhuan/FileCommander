#include "ArchiveBrowserDialog.h"

#include <QAbstractItemView>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>

#include "ThemedDialogs.h"
#include <QPushButton>
#include <QTableView>
#include <QToolBar>
#include <QVBoxLayout>

#include "ArchiveHandler.h"
#include "ArchiveModel.h"

ArchiveBrowserDialog::ArchiveBrowserDialog(const QString &archivePath,
                                            const QString &defaultExtractDir, QWidget *parent)
    : FramelessDialog(parent), m_defaultExtractDir(defaultExtractDir) {
    setWindowTitle(QFileInfo(archivePath).fileName());
    resize(700, 500);

    m_model = new ArchiveModel(this);

    m_pathLabel = new QLabel(this);

    auto *toolbar = new QToolBar(this);
    toolbar->addAction(tr("Up"), this, &ArchiveBrowserDialog::navigateUp);
    toolbar->addAction(tr("Extract Selected"), this,
                        &ArchiveBrowserDialog::extractSelectedToDefault);
    toolbar->addAction(tr("Extract to..."), this,
                        &ArchiveBrowserDialog::extractSelectedToChosenDir);

    m_view = new QTableView(this);
    m_view->setModel(m_model);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->verticalHeader()->hide();
    m_view->horizontalHeader()->setStretchLastSection(false);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);

    connect(m_view, &QAbstractItemView::activated, this, &ArchiveBrowserDialog::onActivated);
    connect(m_view, &QTableView::customContextMenuRequested, this,
            &ArchiveBrowserDialog::showContextMenu);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(toolbar);
    layout->addWidget(m_pathLabel);
    layout->addWidget(m_view, 1);

    QString err;
    if (!m_model->loadArchive(archivePath, &err)) {
        ttc::warning(this, tr("Open Archive"),
                              tr("Could not open %1: %2").arg(archivePath, err));
    }
    m_view->horizontalHeader()->setSectionResizeMode(ArchiveModel::NameColumn,
                                                      QHeaderView::Stretch);
    updatePathLabel();
}

void ArchiveBrowserDialog::updatePathLabel() {
    const QString path = m_model->currentPath();
    m_pathLabel->setText(QStringLiteral("/%1").arg(path));
}

void ArchiveBrowserDialog::onActivated(const QModelIndex &index) {
    if (m_model->isParentEntry(index.row())) {
        navigateUp();
        return;
    }
    auto node = m_model->nodeAt(index.row());
    if (node && node->isDir) {
        m_model->enterDirectory(node->fullPath);
        updatePathLabel();
    }
}

void ArchiveBrowserDialog::navigateUp() {
    if (m_model->navigateUp())
        updatePathLabel();
}

QStringList ArchiveBrowserDialog::selectedEntryPaths() const {
    QStringList paths;
    const QModelIndexList rows = m_view->selectionModel()->selectedRows();
    for (const QModelIndex &idx : rows) {
        if (m_model->isParentEntry(idx.row()))
            continue;
        auto node = m_model->nodeAt(idx.row());
        if (node)
            paths.append(node->fullPath);
    }
    return paths;
}

bool ArchiveBrowserDialog::doExtract(const QString &destDir) {
    const QStringList entries = selectedEntryPaths();

    // Selected entries: plain extraction, preserving their in-archive paths.
    if (!entries.isEmpty()) {
        QString err;
        const bool ok = ArchiveHandler::extract(m_model->archivePath(), entries, destDir, &err);
        if (!ok)
            ttc::warning(this, tr("Extract"), tr("Extraction failed: %1").arg(err));
        else
            ttc::information(
                this, tr("Extract"),
                tr("Extracted %1 item(s) to %2").arg(entries.size()).arg(destDir));
        return ok;
    }

    // Whole archive: Bandizip-style smart layout, then offer to unwrap any single
    // nested archive recursively.
    QString source = m_model->archivePath();
    QString base = destDir;
    QString finalDir;
    for (;;) {
        QString err;
        const ArchiveHandler::SmartResult res = ArchiveHandler::smartExtract(source, base, &err);
        if (!res.ok) {
            ttc::warning(this, tr("Extract"), tr("Extraction failed: %1").arg(err));
            return false;
        }
        finalDir = res.finalDir;
        if (res.nestedArchivePath.isEmpty())
            break;
        const auto answer = ttc::question(
            this, tr("Nested archive"),
            tr("The result contains a single archive:\n%1\n\nExtract it too?")
                .arg(QFileInfo(res.nestedArchivePath).fileName()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (answer != QMessageBox::Yes)
            break;
        source = res.nestedArchivePath;
        base = QFileInfo(res.nestedArchivePath).absolutePath();
    }

    ttc::information(this, tr("Extract"),
                             tr("Extracted archive to %1").arg(finalDir));
    return true;
}

void ArchiveBrowserDialog::extractSelectedToDefault() {
    if (m_defaultExtractDir.isEmpty()) {
        extractSelectedToChosenDir();
        return;
    }
    doExtract(m_defaultExtractDir);
}

void ArchiveBrowserDialog::extractSelectedToChosenDir() {
    const QString dir =
        QFileDialog::getExistingDirectory(this, tr("Extract to"), m_defaultExtractDir);
    if (!dir.isEmpty())
        doExtract(dir);
}

void ArchiveBrowserDialog::showContextMenu(const QPoint &pos) {
    QMenu menu(this);
    menu.addAction(tr("Extract Selected"), this, &ArchiveBrowserDialog::extractSelectedToDefault);
    menu.addAction(tr("Extract to..."), this, &ArchiveBrowserDialog::extractSelectedToChosenDir);
    menu.exec(m_view->viewport()->mapToGlobal(pos));
}
