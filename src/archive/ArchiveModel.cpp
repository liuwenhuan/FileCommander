#include "ArchiveModel.h"

#include <QCollator>

#include "ArchiveHandler.h"

namespace {
QString humanSize(qint64 bytes) {
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double size = static_cast<double>(bytes);
    int unit = 0;
    while (size >= 1024.0 && unit < 4) {
        size /= 1024.0;
        ++unit;
    }
    if (unit == 0)
        return QStringLiteral("%1 B").arg(bytes);
    return QStringLiteral("%1 %2").arg(size, 0, 'f', 1).arg(units[unit]);
}

void sortChildren(QSharedPointer<ArchiveNode> &node) {
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(node->children.begin(), node->children.end(),
              [&](const QSharedPointer<ArchiveNode> &a, const QSharedPointer<ArchiveNode> &b) {
                  if (a->isDir != b->isDir)
                      return a->isDir;
                  return collator.compare(a->name, b->name) < 0;
              });
}
} // namespace

ArchiveModel::ArchiveModel(QObject *parent) : QAbstractTableModel(parent) {}

bool ArchiveModel::loadArchive(const QString &archivePath, QString *errorMessage) {
    return loadArchive(archivePath, QString(), nullptr, errorMessage);
}

bool ArchiveModel::loadArchive(const QString &archivePath, const QString &passphrase,
                               ArchiveHandler::Status *status, QString *errorMessage) {
    ArchiveHandler::Status st = ArchiveHandler::Status::Ok;
    auto root = ArchiveHandler::buildTree(archivePath, passphrase, &st, errorMessage);
    if (status)
        *status = st;
    if (!root)
        return false; // st explains why (NeedPassword / WrongPassword / ...)

    beginResetModel();
    m_archivePath = archivePath;
    m_passphrase = passphrase;
    m_root = root;
    sortChildren(m_root);
    for (auto &child : m_root->children) {
        if (child->isDir)
            sortChildren(child);
    }
    m_currentNode = m_root;
    endResetModel();
    return true;
}

void ArchiveModel::setCurrentNode(QSharedPointer<ArchiveNode> node) {
    if (!node)
        return;
    beginResetModel();
    m_currentNode = node;
    endResetModel();
}

void ArchiveModel::enterDirectory(const QString &fullPath) {
    if (fullPath.isEmpty()) {
        setCurrentNode(m_root);
        return;
    }
    const QStringList parts = fullPath.split('/', Qt::SkipEmptyParts);
    QSharedPointer<ArchiveNode> node = m_root;
    for (const QString &part : parts) {
        node = node->findChild(part);
        if (!node)
            return;
    }
    setCurrentNode(node);
}

bool ArchiveModel::navigateUp() {
    if (isAtRoot() || !m_currentNode->parent)
        return false;
    // parent is a raw pointer into the tree; find the owning QSharedPointer
    // by walking from root via fullPath rather than dereferencing it
    // directly, to keep ownership solely in QSharedPointer hands.
    const QString parentPath = m_currentNode->fullPath.section('/', 0, -2);
    enterDirectory(parentPath);
    return true;
}

QString ArchiveModel::currentPath() const {
    return m_currentNode ? m_currentNode->fullPath : QString();
}

QSharedPointer<ArchiveNode> ArchiveModel::nodeAt(int row) const {
    if (!m_currentNode)
        return {};
    const int offset = isAtRoot() ? 0 : 1;
    const int idx = row - offset;
    if (idx < 0 || idx >= m_currentNode->children.size())
        return {};
    return m_currentNode->children.at(idx);
}

int ArchiveModel::rowCount(const QModelIndex &parent) const {
    if (parent.isValid() || !m_currentNode)
        return 0;
    return m_currentNode->children.size() + (isAtRoot() ? 0 : 1);
}

int ArchiveModel::columnCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant ArchiveModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid())
        return {};

    if (isParentEntry(index.row())) {
        if (role == Qt::DisplayRole && index.column() == NameColumn)
            return QStringLiteral("..");
        if (role == IsDirRole)
            return true;
        return {};
    }

    auto node = nodeAt(index.row());
    if (!node)
        return {};

    if (role == IsDirRole)
        return node->isDir;

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case NameColumn:
            return node->name;
        case SizeColumn:
            return node->isDir ? QStringLiteral("<DIR>") : humanSize(node->size);
        case ModifiedColumn:
            return node->modified.isValid()
                       ? node->modified.toString(QStringLiteral("yyyy-MM-dd HH:mm"))
                       : QString();
        default:
            return {};
        }
    }
    return {};
}

QVariant ArchiveModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return QAbstractTableModel::headerData(section, orientation, role);
    switch (section) {
    case NameColumn:
        return QObject::tr("Name");
    case SizeColumn:
        return QObject::tr("Size");
    case ModifiedColumn:
        return QObject::tr("Modified");
    default:
        return {};
    }
}
