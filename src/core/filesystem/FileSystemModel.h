#pragma once

#include <QAbstractTableModel>
#include <QFutureWatcher>
#include <QHash>
#include <QVector>

#include "FileInfo.h"

// Flat listing of a single directory's contents (not a recursive tree) --
// this backs one FilePanel's file list. Loads asynchronously so opening a
// large directory never blocks the UI thread.
class FileSystemModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column {
        NameColumn = 0,
        ExtColumn,
        SizeColumn,
        ModifiedColumn,
        PermissionsColumn,
        ColumnCount
    };

    enum Role {
        FileInfoRole = Qt::UserRole + 1,
        IsDirRole,
    };

    explicit FileSystemModel(QObject *parent = nullptr);

    void setRootPath(const QString &path);
    QString rootPath() const { return m_rootPath; }
    bool showHiddenFiles() const { return m_showHidden; }
    void setShowHiddenFiles(bool show);

    // Incremental "quick filter": restricts the visible listing to entries
    // whose name matches (case-insensitive substring, or glob if the filter
    // contains * / ?). Empty string shows everything. The ".." entry is never
    // filtered out. Cleared automatically whenever the directory changes.
    void setNameFilter(const QString &filter);
    QString nameFilter() const { return m_nameFilter; }
    static bool matchesFilter(const QString &name, const QString &filter);

    FileInfo fileInfoAt(int row) const;
    bool isParentEntry(int row) const;

    // On-demand directory size: once computed (see directorySize()), the Size
    // column shows the recursive byte total for that folder instead of
    // "<DIR>". Cleared automatically when the directory is rescanned.
    void setComputedDirSize(const QString &path, qint64 bytes);
    static qint64 directorySize(const QString &path);

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    void sort(int column, Qt::SortOrder order) override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;

signals:
    void loadStarted();
    void loadFinished(int count);
    void renameFailed(const QString &message);
    void renamed(const QString &oldPath, const QString &newPath);

private slots:
    void onScanFinished();

private:
    void sortEntries();
    void applyFilter();

    QString m_rootPath;
    bool m_showHidden = false;
    QVector<FileInfo> m_allEntries; // full directory scan (source of truth)
    QVector<FileInfo> m_entries;    // visible subset after quick filter
    QString m_nameFilter;
    QHash<QString, qint64> m_dirSizes; // path -> computed recursive size
    bool m_hasParentEntry = false;
    QFutureWatcher<QVector<FileInfo>> m_watcher;
    int m_sortColumn = NameColumn;
    Qt::SortOrder m_sortOrder = Qt::AscendingOrder;
};
