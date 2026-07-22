#pragma once

#include "FramelessDialog.h"

#include <QKeySequence>
#include <QMap>
#include <QString>

class QTableWidget;
class QLabel;
class QDialogButtonBox;

// Lets the user rebind keyboard shortcuts, with live conflict detection
// (two actions sharing one key sequence disables OK until resolved).
class ShortcutsDialog : public FramelessDialog {
    Q_OBJECT

public:
    // actionLabels: id -> human-readable label, in display order.
    // current/defaults: id -> key sequence.
    ShortcutsDialog(const QList<QPair<QString, QString>> &actionLabels,
                     const QMap<QString, QKeySequence> &current,
                     const QMap<QString, QKeySequence> &defaults, QWidget *parent = nullptr);

    QMap<QString, QKeySequence> resultShortcuts() const { return m_current; }

private slots:
    void onSequenceEdited(int row);
    void restoreDefaults();

private:
    void checkConflicts();

    QTableWidget *m_table;
    QLabel *m_conflictLabel;
    QDialogButtonBox *m_buttons;
    QList<QPair<QString, QString>> m_actionLabels; // id -> label, row-ordered
    QMap<QString, QKeySequence> m_current;
    QMap<QString, QKeySequence> m_defaults;
};
