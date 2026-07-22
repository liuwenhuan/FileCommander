#pragma once

#include <QAbstractTableModel>

#include "ArchiveHandler.h"
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
    // Passphrase-aware load; `status` (out) distinguishes NeedPassword /
    // WrongPassword / EncryptedUnsupported / Error. Returns true only when the
    // listing succeeded (status == Ok).
    bool loadArchive(const QString &archivePath, const QString &passphrase,
                     ArchiveHandler::Status *status, QString *errorMessage = nullptr);
    // Populate the model from an already-built tree (e.g. one produced on a
    // worker thread or served from a cache). Must run on the GUI thread.
    void setTree(QSharedPointer<ArchiveNode> root, const QString &archivePath,
                 const QString &passphrase);
    QString archivePath() const { return m_archivePath; }
    QString passphrase() const { return m_passphrase; } // verified password, if any

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
    QString m_passphrase;
};
