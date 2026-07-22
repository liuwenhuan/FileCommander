#pragma once

#include <QAbstractTableModel>
#include <QFutureWatcher>
#include <QHash>
#include <QStringList>
#include <QVector>

#include <memory>

#include "FileInfo.h"

class FileProvider;

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
        TypeColumn,
        CreatedColumn,
        PermissionsColumn,
        ColumnCount
    };

    // Human-readable file-type category (Folder/Image/Video/Archive/...), by
    // extension. Static for reuse/testing.
    static QString typeCategory(const FileInfo &info);

    enum Role {
        FileInfoRole = Qt::UserRole + 1,
        IsDirRole,
    };

    // Result of a directory comparison, used to colour rows.
    enum CompareStatus { CompareNone, CompareUnique, CompareNewer, CompareOlder };

    explicit FileSystemModel(QObject *parent = nullptr);

    void setRootPath(const QString &path);
    QString rootPath() const { return m_rootPath; }

    // "Flat" listing mode: populate the model with an explicit set of file paths
    // that may span many directories (e.g. Ctrl+F search results shown TC
    // "feed-to-listbox" style) instead of scanning a single directory. There is
    // no ".." entry and the listing is read-only. Any subsequent setRootPath()
    // (normal navigation / refresh) leaves flat mode and restores directory
    // scanning. The Name column shows the full path so cross-directory results
    // stay distinguishable.
    void setFlatEntries(const QStringList &paths);
    bool isFlatMode() const { return m_flatMode; }

    // The backend this model reads through. Defaults to the local filesystem;
    // MainWindow/FilePanel swap in a remote provider when navigating there.
    void setProvider(std::shared_ptr<FileProvider> provider);
    FileProvider *provider() const { return m_provider.get(); }
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

    // Removes the given absolute paths from the listing incrementally (via
    // beginRemoveRows, no directory rescan) so the view keeps its scroll
    // position and can select the row that slid into the gap. Returns the
    // smallest visible row index that was removed (the "select next" anchor),
    // or -1 if nothing matched. Used after a delete instead of a full refresh.
    int removePaths(const QStringList &paths);

    // On-demand directory size: once computed (see directorySize()), the Size
    // column shows the recursive byte total for that folder instead of
    // "<DIR>". Cleared automatically when the directory is rescanned.
    void setComputedDirSize(const QString &path, qint64 bytes);
    static qint64 directorySize(const QString &path);

    // Colours rows per a name->CompareStatus map (from "Compare Directories").
    // Cleared automatically on rescan.
    void setCompareStatus(const QHash<QString, int> &statusByName);
    void clearCompareStatus();

    // Re-emits header + cell change signals so views re-query tr()'d text (column
    // titles, the type column) after a live UI-language switch. No rescan.
    void retranslate();

    // Pure comparison: each name in `self` is Unique (absent from `other`),
    // Newer, or Older based on modification times. Static for unit testing.
    static QHash<QString, int> compareStatuses(const QHash<QString, QDateTime> &self,
                                                const QHash<QString, QDateTime> &other);

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

    std::shared_ptr<FileProvider> m_provider; // backend (local by default); never null
    QString m_rootPath;
    bool m_flatMode = false; // true while showing an explicit cross-directory path list
    bool m_showHidden = false;
    QVector<FileInfo> m_allEntries; // full directory scan (source of truth)
    QVector<FileInfo> m_entries;    // visible subset after quick filter
    QString m_nameFilter;
    QHash<QString, qint64> m_dirSizes;    // path -> computed recursive size
    QHash<QString, int> m_compareStatus;  // name -> CompareStatus
    bool m_hasParentEntry = false;
    QFutureWatcher<QVector<FileInfo>> m_watcher;
    int m_sortColumn = NameColumn;
    Qt::SortOrder m_sortOrder = Qt::AscendingOrder;
};
