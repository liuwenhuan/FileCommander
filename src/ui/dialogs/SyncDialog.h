#pragma once

#include "FramelessDialog.h"
#include <QVector>

#include "DirectorySync.h"

class QTableWidget;
class QCheckBox;
class QPushButton;
class QLabel;
class OperationQueue;

// Commands > Synchronize Directories: compares the two panels' current
// directories and lets the user selectively (or in bulk) copy
// differences in either direction.
class SyncDialog : public FramelessDialog {
    Q_OBJECT

public:
    SyncDialog(const QString &leftDir, const QString &rightDir, QWidget *parent = nullptr);

private slots:
    void refresh();
    void copySelected(bool leftToRight);

private:
    void populateTable();
    QStringList selectedRelativePaths() const;

    QString m_leftDir;
    QString m_rightDir;
    QVector<SyncEntry> m_entries;

    QTableWidget *m_table;
    QCheckBox *m_recursiveCheck;
    QCheckBox *m_hideIdenticalCheck;
    QLabel *m_summaryLabel;
    OperationQueue *m_queue;
};
