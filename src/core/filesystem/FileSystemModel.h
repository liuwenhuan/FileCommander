#pragma once

#include <QAbstractTableModel>
#include <QFutureWatcher>
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

    FileInfo fileInfoAt(int row) const;
    bool isParentEntry(int row) const;

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    void sort(int column, Qt::SortOrder order) override;

signals:
    void loadStarted();
    void loadFinished(int count);

private slots:
    void onScanFinished();

private:
    void sortEntries();

    QString m_rootPath;
    bool m_showHidden = false;
    QVector<FileInfo> m_entries;
    bool m_hasParentEntry = false;
    QFutureWatcher<QVector<FileInfo>> m_watcher;
    int m_sortColumn = NameColumn;
    Qt::SortOrder m_sortOrder = Qt::AscendingOrder;
};
