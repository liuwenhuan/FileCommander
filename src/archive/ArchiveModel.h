#pragma once

#include <QAbstractTableModel>

#include "ArchiveNode.h"

// Flat listing of one "directory" inside an archive's in-memory tree --
// mirrors FileSystemModel's interaction model (rows + a ".." entry) so
// ArchiveBrowserDialog can reuse the same double-click-to-navigate feel.
class ArchiveModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column { NameColumn = 0, SizeColumn, ModifiedColumn, ColumnCount };
    enum Role { IsDirRole = Qt::UserRole + 1 };

    explicit ArchiveModel(QObject *parent = nullptr);

    bool loadArchive(const QString &archivePath, QString *errorMessage = nullptr);
    QString archivePath() const { return m_archivePath; }

    void enterDirectory(const QString &fullPath); // empty string = root
    bool navigateUp();
    QString currentPath() const;
    bool isAtRoot() const { return m_currentNode == m_root; }

    QSharedPointer<ArchiveNode> nodeAt(int row) const;
    bool isParentEntry(int row) const { return !isAtRoot() && row == 0; }

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    void setCurrentNode(QSharedPointer<ArchiveNode> node);

    QSharedPointer<ArchiveNode> m_root;
    QSharedPointer<ArchiveNode> m_currentNode;
    QString m_archivePath;
};
