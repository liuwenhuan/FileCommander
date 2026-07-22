#include "SyncDialog.h"

#include <QCheckBox>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>

#include "ThemedDialogs.h"
#include <QPushButton>
#include <QSet>
#include <QTableWidget>
#include <QVBoxLayout>

#include "OperationQueue.h"
#include "OverwriteConfirmDialog.h"

namespace {
QString statusLabel(SyncEntry::Status status) {
    switch (status) {
    case SyncEntry::Status::LeftOnly:
        return QObject::tr("Left only");
    case SyncEntry::Status::RightOnly:
        return QObject::tr("Right only");
    case SyncEntry::Status::Different:
        return QObject::tr("Different");
    case SyncEntry::Status::Same:
        return QObject::tr("Same");
    }
    return {};
}

QString sizeOrDash(qint64 size) {
    return size < 0 ? QStringLiteral("-") : QString::number(size);
}

QString timeOrDash(const QDateTime &dt) {
    return dt.isValid() ? dt.toString(QStringLiteral("yyyy-MM-dd HH:mm")) : QStringLiteral("-");
}
} // namespace

SyncDialog::SyncDialog(const QString &leftDir, const QString &rightDir, QWidget *parent)
    : FramelessDialog(parent), m_leftDir(leftDir), m_rightDir(rightDir) {
    setWindowTitle(tr("Synchronize Directories"));
    resize(900, 600);

    m_queue = new OperationQueue(this);
    m_queue->setConflictHandler([this](const QString &src, const QString &dst) {
        return OverwriteConfirmDialog::ask(this, src, dst);
    });
    connect(m_queue, &OperationQueue::finished, this, [this](bool) { refresh(); });
    connect(m_queue, &OperationQueue::errorOccurred, this, [this](const QString &msg) {
        ttc::warning(this, tr("Synchronize"), msg);
    });

    auto *pathsLabel =
        new QLabel(tr("Left: %1\nRight: %2").arg(leftDir, rightDir), this);

    m_recursiveCheck = new QCheckBox(tr("Include subdirectories"), this);
    m_recursiveCheck->setChecked(true);
    m_hideIdenticalCheck = new QCheckBox(tr("Hide identical files"), this);
    m_hideIdenticalCheck->setChecked(true);
    connect(m_recursiveCheck, &QCheckBox::toggled, this, &SyncDialog::refresh);
    connect(m_hideIdenticalCheck, &QCheckBox::toggled, this, &SyncDialog::populateTable);

    auto *refreshButton = new QPushButton(tr("Refresh"), this);
    connect(refreshButton, &QPushButton::clicked, this, &SyncDialog::refresh);

    auto *optionsRow = new QHBoxLayout;
    optionsRow->addWidget(m_recursiveCheck);
    optionsRow->addWidget(m_hideIdenticalCheck);
    optionsRow->addStretch(1);
    optionsRow->addWidget(refreshButton);

    m_table = new QTableWidget(0, 5, this);
    m_table->setHorizontalHeaderLabels(
        {tr("Status"), tr("Relative Path"), tr("Left Size"), tr("Right Size"), tr("Modified")});
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->verticalHeader()->hide();
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);

    auto *copyRightButton = new QPushButton(tr("Copy Selected: Left → Right"), this);
    auto *copyLeftButton = new QPushButton(tr("Copy Selected: Right → Left"), this);
    connect(copyRightButton, &QPushButton::clicked, this,
            [this]() { copySelected(/*leftToRight=*/true); });
    connect(copyLeftButton, &QPushButton::clicked, this,
            [this]() { copySelected(/*leftToRight=*/false); });

    auto *actionsRow = new QHBoxLayout;
    actionsRow->addWidget(copyRightButton);
    actionsRow->addWidget(copyLeftButton);
    actionsRow->addStretch(1);

    m_summaryLabel = new QLabel(this);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(pathsLabel);
    layout->addLayout(optionsRow);
    layout->addWidget(m_table, 1);
    layout->addWidget(m_summaryLabel);
    layout->addLayout(actionsRow);

    refresh();
}

void SyncDialog::refresh() {
    m_entries = DirectorySync::compare(m_leftDir, m_rightDir, m_recursiveCheck->isChecked());
    populateTable();
}

void SyncDialog::populateTable() {
    const bool hideIdentical = m_hideIdenticalCheck->isChecked();
    QVector<int> visibleIndices;
    for (int i = 0; i < m_entries.size(); ++i) {
        if (!hideIdentical || m_entries.at(i).status != SyncEntry::Status::Same)
            visibleIndices.append(i);
    }

    m_table->setRowCount(visibleIndices.size());
    int leftOnly = 0, rightOnly = 0, different = 0;

    for (int row = 0; row < visibleIndices.size(); ++row) {
        const SyncEntry &entry = m_entries.at(visibleIndices.at(row));
        m_table->setItem(row, 0, new QTableWidgetItem(statusLabel(entry.status)));
        m_table->setItem(row, 1, new QTableWidgetItem(entry.relativePath));
        m_table->setItem(row, 2, new QTableWidgetItem(sizeOrDash(entry.leftSize)));
        m_table->setItem(row, 3, new QTableWidgetItem(sizeOrDash(entry.rightSize)));
        const QDateTime newest = entry.leftModified > entry.rightModified ? entry.leftModified
                                                                            : entry.rightModified;
        m_table->setItem(row, 4, new QTableWidgetItem(timeOrDash(newest)));

        // Row 0 (Status) drives the item data used by selectedRelativePaths().
        m_table->item(row, 0)->setData(Qt::UserRole, visibleIndices.at(row));

        switch (entry.status) {
        case SyncEntry::Status::LeftOnly:
            ++leftOnly;
            break;
        case SyncEntry::Status::RightOnly:
            ++rightOnly;
            break;
        case SyncEntry::Status::Different:
            ++different;
            break;
        case SyncEntry::Status::Same:
            break;
        }
    }

    m_summaryLabel->setText(tr("%1 left only, %2 right only, %3 different")
                                 .arg(leftOnly)
                                 .arg(rightOnly)
                                 .arg(different));
}

QStringList SyncDialog::selectedRelativePaths() const {
    QStringList result;
    QSet<int> seenRows;
    for (const QModelIndex &idx : m_table->selectionModel()->selectedIndexes()) {
        if (seenRows.contains(idx.row()))
            continue;
        seenRows.insert(idx.row());
        const int entryIndex = m_table->item(idx.row(), 0)->data(Qt::UserRole).toInt();
        result.append(m_entries.at(entryIndex).relativePath);
    }
    return result;
}

void SyncDialog::copySelected(bool leftToRight) {
    const QStringList relPaths = selectedRelativePaths();
    if (relPaths.isEmpty())
        return;

    const QString &srcBase = leftToRight ? m_leftDir : m_rightDir;
    const QString &dstBase = leftToRight ? m_rightDir : m_leftDir;

    for (const QString &rel : relPaths) {
        const QString srcPath = QDir(srcBase).filePath(rel);
        if (!QFileInfo::exists(srcPath))
            continue; // e.g. copying a "right only" file leftToRight makes no sense
        const QString destDir = QDir(dstBase).filePath(QFileInfo(rel).path());
        m_queue->enqueueCopy({srcPath}, destDir);
    }
}
