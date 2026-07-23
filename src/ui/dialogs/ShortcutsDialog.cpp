#include "ShortcutsDialog.h"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

ShortcutsDialog::ShortcutsDialog(const QList<QPair<QString, QString>> &actionLabels,
                                  const QMap<QString, QKeySequence> &current,
                                  const QMap<QString, QKeySequence> &defaults, QWidget *parent)
    : FramelessDialog(parent), m_actionLabels(actionLabels), m_current(current),
      m_defaults(defaults) {
    setWindowTitle(tr("Keyboard Shortcuts"));
    resize(500, 500);

    m_table = new QTableWidget(actionLabels.size(), 2, this);
    m_table->setHorizontalHeaderLabels({tr("Command"), tr("Shortcut")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->verticalHeader()->hide();
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);

    for (int row = 0; row < actionLabels.size(); ++row) {
        const QString &id = actionLabels.at(row).first;
        const QString &label = actionLabels.at(row).second;

        auto *labelItem = new QTableWidgetItem(label);
        labelItem->setFlags(labelItem->flags() & ~Qt::ItemIsEditable);
        m_table->setItem(row, 0, labelItem);

        auto *editor = new QKeySequenceEdit(current.value(id), m_table);
        m_table->setCellWidget(row, 1, editor);
        connect(editor, &QKeySequenceEdit::editingFinished, this,
                [this, row]() { onSequenceEdited(row); });
    }

    m_conflictLabel = new QLabel(this);
    m_conflictLabel->setStyleSheet(QStringLiteral("color: #c0392b;"));

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto *restoreButton = m_buttons->addButton(tr("Restore Defaults"), QDialogButtonBox::ResetRole);
    connect(restoreButton, &QPushButton::clicked, this, &ShortcutsDialog::restoreDefaults);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_table, 1);
    layout->addWidget(m_conflictLabel);
    layout->addWidget(m_buttons);
}

void ShortcutsDialog::onSequenceEdited(int row) {
    auto *editor = qobject_cast<QKeySequenceEdit *>(m_table->cellWidget(row, 1));
    if (!editor)
        return;
    const QString id = m_actionLabels.at(row).first;
    m_current[id] = editor->keySequence();
    checkConflicts();
}

void ShortcutsDialog::checkConflicts() {
    QMap<QString, QStringList> bySequence; // sequence string -> action ids sharing it
    for (auto it = m_current.constBegin(); it != m_current.constEnd(); ++it) {
        if (it.value().isEmpty())
            continue;
        bySequence[it.value().toString()].append(it.key());
    }

    QStringList conflictingIds;
    for (auto it = bySequence.constBegin(); it != bySequence.constEnd(); ++it) {
        if (it.value().size() > 1)
            conflictingIds.append(it.value());
    }

    if (conflictingIds.isEmpty()) {
        m_conflictLabel->clear();
        m_buttons->button(QDialogButtonBox::Ok)->setEnabled(true);
        return;
    }

    QStringList labels;
    for (const QString &id : conflictingIds) {
        for (const auto &pair : m_actionLabels) {
            if (pair.first == id)
                labels.append(pair.second);
        }
    }
    m_conflictLabel->setText(tr("Conflicting shortcut(s): %1").arg(labels.join(", ")));
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
}

void ShortcutsDialog::restoreDefaults() {
    m_current = m_defaults;
    for (int row = 0; row < m_actionLabels.size(); ++row) {
        const QString id = m_actionLabels.at(row).first;
        auto *editor = qobject_cast<QKeySequenceEdit *>(m_table->cellWidget(row, 1));
        if (editor)
            editor->setKeySequence(m_defaults.value(id));
    }
    checkConflicts();
}
