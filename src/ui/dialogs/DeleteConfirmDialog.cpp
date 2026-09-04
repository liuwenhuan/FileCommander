#include "DeleteConfirmDialog.h"

#include "DirectoryStatisticsTask.h"
#include "FileSystemModel.h"
#include "ThemedDialogs.h"

#include <QDialogButtonBox>
#include <QHash>
#include <QLabel>
#include <QLocale>
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

} // namespace

DeleteSelectionSummary summarizeDeleteSelection(const FileSystemModel *model,
                                                const QStringList &paths) {
    DeleteSelectionSummary summary;
    if (!model)
        return summary;

    QHash<QString, FileInfo> byPath;
    const int rows = model->rowCount();
    byPath.reserve(rows);
    for (int row = 0; row < rows; ++row) {
        const FileInfo info = model->fileInfoAt(row);
        byPath.insert(info.path(), info);
    }

    for (const QString &path : paths) {
        const auto it = byPath.constFind(path);
        if (it == byPath.constEnd()) {
            // Not in the listing (a stale selection, or the current-row
            // fallback on a row that has gone): count it, do not guess a size.
            ++summary.fileCount;
            continue;
        }
        if (it->isDir()) {
            ++summary.folderCount;
        } else {
            ++summary.fileCount;
            summary.listedBytes += it->size();
        }
    }
    return summary;
}

DeleteConfirmDialog::DeleteConfirmDialog(const QStringList &paths,
                                         const DeleteSelectionSummary &summary, bool permanent,
                                         bool measureLocally, QWidget *parent)
    : FramelessDialog(parent), m_summary(summary), m_measureLocally(measureLocally) {
    setWindowTitle(tr("Confirm Delete"));
    setModal(true);

    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setObjectName(QStringLiteral("DeleteConfirmSummary"));
    m_summaryLabel->setWordWrap(true);
    m_summaryLabel->setText(summaryText());

    m_sizeLabel = new QLabel(this);
    m_sizeLabel->setObjectName(QStringLiteral("DeleteConfirmSize"));
    m_sizeLabel->setWordWrap(true);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_summaryLabel);
    layout->addWidget(m_sizeLabel);

    if (permanent) {
        auto *warning = new QLabel(
            tr("This is permanent and will NOT go to the trash."), this);
        warning->setObjectName(QStringLiteral("DeleteConfirmWarning"));
        warning->setWordWrap(true);
        layout->addWidget(warning);
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Yes | QDialogButtonBox::No, this);
    ttc::localizeStandardButtons(buttons);
    QPushButton *yes = buttons->button(QDialogButtonBox::Yes);
    QPushButton *no = buttons->button(QDialogButtonBox::No);
    connect(yes, &QPushButton::clicked, this, &QDialog::accept);
    connect(no, &QPushButton::clicked, this, &QDialog::reject);
    // Keep the intended action visually identifiable even on Windows styles
    // that do not paint QPushButton::default reliably.
    yes->setProperty("ttcDefaultButton", true);
    yes->setDefault(true);
    yes->setFocus(Qt::OtherFocusReason);
    layout->addWidget(buttons);

    // Show what is known straight away, then refine it. Only folders need the
    // walk; a selection of plain files is already exact, and one that cannot be
    // walked has to say that rather than present a partial figure as the total.
    const bool needsWalk = measureLocally && summary.folderCount > 0;
    showSize(summary.listedBytes,
             summary.folderCount == 0
                 ? SizeState::Exact
                 : (needsWalk ? SizeState::Measuring : SizeState::FilesOnly));
    if (!needsWalk)
        return;

    m_measuring = true;
    m_task = new DirectoryStatisticsTask(paths, this);
    connect(m_task, &DirectoryStatisticsTask::finished, this,
            [this](qint64 bytes, qint64, bool cancelled) {
                m_measuring = false;
                if (!cancelled)
                    showSize(bytes, SizeState::Exact);
                emit measurementFinished();
            });
    m_task->start();
}

void DeleteConfirmDialog::showSize(qint64 bytes, SizeState state) {
    QString text;
    switch (state) {
    case SizeState::Exact:
        text = tr("Size: %1 (%2 bytes)").arg(humanBytes(bytes), QLocale().toString(bytes));
        break;
    case SizeState::Measuring:
        // The figure so far covers the selected files; the folders are being
        // walked. Saying so beats showing a number that looks final.
        text = tr("Size: %1 so far — measuring the selected folders…").arg(humanBytes(bytes));
        break;
    case SizeState::FilesOnly:
        text = tr("Size: %1 — selected files only, folder contents are not counted here")
                   .arg(humanBytes(bytes));
        break;
    }
    m_sizeLabel->setText(text);
}

QString DeleteConfirmDialog::summaryText() const {
    const int total = m_summary.fileCount + m_summary.folderCount;
    if (m_summary.folderCount == 0)
        return tr("Delete %n file(s)?", nullptr, m_summary.fileCount);
    if (m_summary.fileCount == 0)
        return tr("Delete %n folder(s) and everything in them?", nullptr,
                  m_summary.folderCount);
    return tr("Delete %1 items — %2 and %3, including everything in the folders?")
        .arg(total)
        .arg(tr("%n file(s)", nullptr, m_summary.fileCount))
        .arg(tr("%n folder(s)", nullptr, m_summary.folderCount));
}

QString DeleteConfirmDialog::sizeText() const {
    return m_sizeLabel ? m_sizeLabel->text() : QString();
}

bool DeleteConfirmDialog::ask(QWidget *parent, const QStringList &paths,
                              const DeleteSelectionSummary &summary, bool permanent,
                              bool measureLocally) {
    DeleteConfirmDialog dialog(paths, summary, permanent, measureLocally, parent);
    return dialog.exec() == QDialog::Accepted;
}
