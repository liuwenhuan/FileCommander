#pragma once

#include <QAbstractTableModel>
#include <QHash>
#include <QVector>

#include "DirectorySync.h"

// Table model behind the synchronize dialog's two-pane view.
//
// The seven columns lay the two directories out side by side with the action
// column between them, so left and right read as two panes while remaining a
// single model -- two independent views would have to keep their scroll
// positions in sync manually and drift apart the moment row heights differ.
//
// The model separates what the *comparison* found (SyncEntry::Status, fixed once
// the scanner emits it) from what will *happen* (Direction, which the user can
// override per row). The scanner's classification never changes underfoot; only
// the direction does.
class SyncModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column {
        LeftNameColumn = 0,
        LeftSizeColumn,
        LeftTimeColumn,
        ActionColumn,
        RightNameColumn,
        RightSizeColumn,
        RightTimeColumn,
        ColumnCount
    };

    // What the sync run will do with a row. Derived from the comparison, then
    // freely overridable by the user (clicking the action cell cycles it).
    enum class Direction {
        None,        // nothing to do: the two sides already match
        ToRight,     // copy left -> right
        ToLeft,      // copy right -> left
        Conflict,    // both sides differ with no clear winner; needs a decision
        Skip         // explicitly excluded by the user
    };

    struct Row {
        SyncEntry entry;
        Direction direction = Direction::None;
    };

    explicit SyncModel(QObject *parent = nullptr);

    // Appends a batch from the scanner in one beginInsertRows, so a large tree
    // streams in without a per-row view reset.
    void appendEntries(const QVector<SyncEntry> &entries);
    void clearRows();

    // Takes a *visible* row index (what a view hands out), not an index into the
    // full result set -- the two differ whenever identical rows are hidden.
    Direction directionAt(int visibleRow) const;
    int totalRowCount() const { return m_rows.size(); }

    // Rows the sync run would actually copy (direction ToRight/ToLeft), in
    // model order.
    QVector<Row> actionableRows() const;

    // Cycles a row's direction through the choices that make sense for its
    // status. A left-only file can only go left->right (or be skipped), so the
    // cycle never offers an impossible direction.
    void cycleDirection(int row);
    void setDirection(int row, Direction direction);
    // Bulk helpers behind the toolbar buttons. Only rows the comparison found a
    // difference for are touched; identical rows stay at None.
    void setAllDirections(Direction direction);

    // Hides rows whose status is Same. Identical files are noise by default:
    // the point of the window is the differences.
    void setShowIdentical(bool show);
    bool showIdentical() const { return m_showIdentical; }

    struct Summary {
        int toRight = 0;
        int toLeft = 0;
        int conflicts = 0;
        int identical = 0;
        int skipped = 0;
        qint64 bytesToCopy = 0;
    };
    Summary summary() const { return m_summary; }

    // Human-readable explanation of a row's action, used for the tooltip. The
    // conflict case in particular needs plain words: "not equal" alone doesn't
    // tell anyone why the row refuses to pick a side.
    QString explain(int row) const;

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

signals:
    void summaryChanged();

private:
    // The direction the comparison implies, before any user override.
    static Direction defaultDirection(const SyncEntry &entry);
    void recomputeSummary();
    // Maps a visible row index to its index in m_rows (they differ only when
    // identical rows are hidden).
    int sourceRow(int visibleRow) const;
    void rebuildVisible();

    QVector<Row> m_rows;
    QVector<int> m_visible; // indices into m_rows
    bool m_showIdentical = false;
    Summary m_summary;
};
