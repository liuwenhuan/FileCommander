#include "FileSystemModel.h"

#include <QCollator>
#include <QColor>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>
#include <QtConcurrent/QtConcurrent>

#include "FileProvider.h"
#include "IconCache.h"
#include "LocalFileProvider.h"

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

} // namespace

namespace {
// The local provider is a process-wide singleton; wrap it in a shared_ptr with
// a no-op deleter so the model can hold every provider uniformly by shared_ptr.
std::shared_ptr<FileProvider> localProviderPtr() {
    return std::shared_ptr<FileProvider>(LocalFileProvider::instance(), [](FileProvider *) {});
}
} // namespace

FileSystemModel::FileSystemModel(QObject *parent) : QAbstractTableModel(parent) {
    m_provider = localProviderPtr();
    connect(&m_watcher, &QFutureWatcher<QVector<FileInfo>>::finished, this,
            &FileSystemModel::onScanFinished);
}

void FileSystemModel::setProvider(std::shared_ptr<FileProvider> provider) {
    m_provider = provider ? std::move(provider) : localProviderPtr();
}

void FileSystemModel::setRootPath(const QString &path) {
    m_flatMode = false;       // leaving any flat search-results listing
    m_rootPath = path;
    m_nameFilter.clear();     // a fresh directory always starts unfiltered
    m_dirSizes.clear();       // computed folder sizes don't survive a rescan
    m_compareStatus.clear();  // comparison highlights are stale after a rescan
    m_dateStrCache.clear();   // bound the memo; new listing, new timestamps
    emit loadStarted();
    // Capture a shared_ptr copy so the provider outlives this worker-thread scan
    // even if the model switches to another provider meanwhile.
    std::shared_ptr<FileProvider> provider = m_provider;
    const bool showHidden = m_showHidden;
    QFuture<QVector<FileInfo>> future =
        QtConcurrent::run([provider, path, showHidden] { return provider->list(path, showHidden); });
    m_watcher.setFuture(future);
}

void FileSystemModel::setShowHiddenFiles(bool show) {
    if (m_showHidden == show)
        return;
    m_showHidden = show;
    if (!m_rootPath.isEmpty())
        setRootPath(m_rootPath);
}

void FileSystemModel::setFlatEntries(const QStringList &paths) {
    beginResetModel();
    m_flatMode = true;
    m_rootPath.clear();      // no single directory backs a flat listing
    m_nameFilter.clear();
    m_dirSizes.clear();
    m_compareStatus.clear();
    m_hasParentEntry = false; // cross-directory list has no single ".." parent
    m_allEntries.clear();
    m_allEntries.reserve(paths.size());
    for (const QString &p : paths) {
        FileInfo info(p);
        if (info.isValid())
            m_allEntries.append(info);
    }
    sortEntries();           // sorts m_allEntries and rebuilds the visible m_entries
    endResetModel();
    emit loadFinished(m_entries.size());
}

void FileSystemModel::onScanFinished() {
    // A directory scan started before we switched to flat mode may still be
    // running; ignore its late result so it doesn't clobber the flat listing.
    if (m_flatMode)
        return;
    beginResetModel();
    m_allEntries = m_watcher.result();
    m_hasParentEntry = !m_provider->parentPath(m_rootPath).isEmpty();
    sortEntries(); // sorts m_allEntries and rebuilds the visible m_entries
    endResetModel();
    emit loadFinished(m_entries.size());
}

bool FileSystemModel::matchesFilter(const QString &name, const QString &filter) {
    if (filter.isEmpty())
        return true;
    if (filter.contains(QLatin1Char('*')) || filter.contains(QLatin1Char('?'))) {
        const QRegularExpression re(QRegularExpression::wildcardToRegularExpression(filter),
                                     QRegularExpression::CaseInsensitiveOption);
        return re.match(name).hasMatch();
    }
    return name.contains(filter, Qt::CaseInsensitive);
}

void FileSystemModel::applyFilter() {
    if (m_nameFilter.isEmpty()) {
        m_entries = m_allEntries;
        return;
    }
    m_entries.clear();
    m_entries.reserve(m_allEntries.size());
    for (const FileInfo &entry : m_allEntries)
        if (matchesFilter(entry.name(), m_nameFilter))
            m_entries.append(entry);
}

void FileSystemModel::setNameFilter(const QString &filter) {
    if (m_nameFilter == filter)
        return;
    beginResetModel();
    m_nameFilter = filter;
    applyFilter();
    endResetModel();
}

qint64 FileSystemModel::directorySize(const QString &path) {
    qint64 total = 0;
    QDirIterator it(path, QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                     QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        total += it.fileInfo().size();
    }
    return total;
}

void FileSystemModel::setCompareStatus(const QHash<QString, int> &statusByName) {
    beginResetModel();
    m_compareStatus = statusByName;
    endResetModel();
}

QHash<QString, int> FileSystemModel::compareStatuses(const QHash<QString, QDateTime> &self,
                                                      const QHash<QString, QDateTime> &other) {
    QHash<QString, int> result;
    for (auto it = self.constBegin(); it != self.constEnd(); ++it) {
        if (!other.contains(it.key()))
            result.insert(it.key(), CompareUnique);
        else if (it.value() > other.value(it.key()))
            result.insert(it.key(), CompareNewer);
        else
            result.insert(it.key(), CompareOlder);
    }
    return result;
}

void FileSystemModel::clearCompareStatus() {
    if (m_compareStatus.isEmpty())
        return;
    beginResetModel();
    m_compareStatus.clear();
    endResetModel();
}

void FileSystemModel::setComputedDirSize(const QString &path, qint64 bytes) {
    m_dirSizes.insert(path, bytes);
    // Repaint the Size cell of the matching visible row, if it's shown.
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries.at(i).path() == path) {
            const int row = m_hasParentEntry ? i + 1 : i;
            const QModelIndex idx = index(row, SizeColumn);
            emit dataChanged(idx, idx, {Qt::DisplayRole});
            break;
        }
    }
}

bool FileSystemModel::isParentEntry(int row) const {
    return m_hasParentEntry && row == 0;
}

FileInfo FileSystemModel::fileInfoAt(int row) const {
    if (isParentEntry(row))
        return FileInfo::makeParentEntry(m_provider->parentPath(m_rootPath));
    int idx = m_hasParentEntry ? row - 1 : row;
    if (idx < 0 || idx >= m_entries.size())
        return FileInfo();
    return m_entries.at(idx);
}

int FileSystemModel::removePaths(const QStringList &paths) {
    if (paths.isEmpty())
        return -1;
    const QSet<QString> targets(paths.begin(), paths.end());
    int anchorRow = -1;
    // Walk the visible list back-to-front so earlier row indices stay valid as
    // we splice each match out. The last (lowest) row we touch is the anchor.
    for (int i = m_entries.size() - 1; i >= 0; --i) {
        if (!targets.contains(m_entries.at(i).path()))
            continue;
        const int row = m_hasParentEntry ? i + 1 : i;
        beginRemoveRows(QModelIndex(), row, row);
        m_entries.remove(i);
        endRemoveRows();
        anchorRow = row;
    }
    // Keep the unfiltered backing store in sync so a later sort/filter/rescan
    // boundary doesn't resurrect the removed entries.
    for (int i = m_allEntries.size() - 1; i >= 0; --i)
        if (targets.contains(m_allEntries.at(i).path()))
            m_allEntries.remove(i);
    return anchorRow;
}

int FileSystemModel::rowCount(const QModelIndex &parent) const {
    if (parent.isValid())
        return 0;
    return m_entries.size() + (m_hasParentEntry ? 1 : 0);
}

int FileSystemModel::columnCount(const QModelIndex &parent) const {
    if (parent.isValid())
        return 0;
    return ColumnCount;
}

QString FileSystemModel::cachedDateStr(const QDateTime &dt) const {
    if (!dt.isValid())
        return {};
    // Key by whole minutes -- the format has no seconds, so timestamps in the
    // same minute share a string.
    const qint64 key = dt.toMSecsSinceEpoch() / 60000;
    auto it = m_dateStrCache.constFind(key);
    if (it != m_dateStrCache.constEnd())
        return it.value();
    const QString s = dt.toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    m_dateStrCache.insert(key, s);
    return s;
}

QVariant FileSystemModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid())
        return {};

    // Hot path: data() is called several times per cell on every repaint, so
    // bind a reference into m_entries instead of copying a (QString/QDateTime-
    // heavy) FileInfo each time. Only the rare parent ".." row needs a
    // constructed value.
    const int row = index.row();
    FileInfo parentHolder;
    const FileInfo *infoPtr;
    if (isParentEntry(row)) {
        parentHolder = FileInfo::makeParentEntry(m_provider->parentPath(m_rootPath));
        infoPtr = &parentHolder;
    } else {
        const int idx = m_hasParentEntry ? row - 1 : row;
        if (idx < 0 || idx >= m_entries.size())
            return {};
        infoPtr = &m_entries.at(idx);
    }
    const FileInfo &info = *infoPtr;
    if (!info.isValid())
        return {};

    if (role == FileInfoRole)
        return QVariant::fromValue(info.path());
    if (role == IsDirRole)
        return info.isDir();
    if (role == Qt::DecorationRole && index.column() == NameColumn)
        return IconCache::instance().iconFor(info);

    if (role == Qt::DisplayRole || role == Qt::EditRole) {
        switch (index.column()) {
        case NameColumn:
            // Flat search-results listing shows the full path so results from
            // different directories stay distinguishable; a normal directory
            // listing shows the base name only (extension is its own column).
            return m_flatMode ? info.path() : info.baseName();
        case ExtColumn:
            return info.isDir() ? QString() : info.suffix();
        case SizeColumn:
            if (info.isDir()) {
                auto it = m_dirSizes.constFind(info.path());
                return it != m_dirSizes.constEnd() ? humanSize(it.value())
                                                     : QStringLiteral("<DIR>");
            }
            return humanSize(info.size());
        case ModifiedColumn:
            return cachedDateStr(info.modified());
        case CreatedColumn:
            return info.created().isValid() ? cachedDateStr(info.created()) : QString();
        case TypeColumn:
            return typeCategory(info);
        case PermissionsColumn:
            return info.permissionsString();
        default:
            return {};
        }
    }

    if (role == Qt::TextAlignmentRole && index.column() == SizeColumn)
        return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);

    if (role == Qt::ForegroundRole && !m_compareStatus.isEmpty()) {
        switch (m_compareStatus.value(info.name(), CompareNone)) {
        case CompareNewer:
            return QColor(0xc0, 0x39, 0x2b); // red: newer than the other panel
        case CompareUnique:
            return QColor(0x27, 0x7a, 0x46); // green: only on this side
        default:
            break;
        }
    }

    return {};
}

void FileSystemModel::retranslate() {
    // Column titles are tr()'d in headerData(); the type column's text is tr()'d
    // in data(). Re-emit so the views re-query both in the new language.
    emit headerDataChanged(Qt::Horizontal, 0, columnCount() - 1);
    if (!m_entries.isEmpty())
        emit dataChanged(index(0, 0), index(m_entries.size() - 1, columnCount() - 1),
                         {Qt::DisplayRole});
}

QVariant FileSystemModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return QAbstractTableModel::headerData(section, orientation, role);

    switch (section) {
    case NameColumn:
        return QObject::tr("Name");
    case ExtColumn:
        return QObject::tr("Ext");
    case SizeColumn:
        return QObject::tr("Size");
    case ModifiedColumn:
        return QObject::tr("Modified");
    case CreatedColumn:
        return QObject::tr("Created");
    case TypeColumn:
        return QObject::tr("Type");
    case PermissionsColumn:
        return QObject::tr("Permissions");
    default:
        return {};
    }
}

QString FileSystemModel::typeCategory(const FileInfo &info) {
    if (info.isDir())
        return QObject::tr("Folder");
    const QString ext = info.suffix().toLower();
    if (ext.isEmpty())
        return QObject::tr("File");

    static const QSet<QString> images = {"jpg", "jpeg", "png",  "gif",  "bmp", "webp",
                                          "svg", "tiff", "tif",  "ico",  "heic"};
    static const QSet<QString> videos = {"mp4", "mkv",  "avi",  "mov", "wmv",
                                          "flv", "webm", "m4v",  "mpg", "mpeg", "ts"};
    static const QSet<QString> audios = {"mp3", "wav", "flac", "ogg", "aac", "m4a", "wma", "opus"};
    static const QSet<QString> archives = {"zip", "rar", "7z",  "tar", "gz",
                                            "bz2", "xz",  "tgz", "zst", "lz"};
    static const QSet<QString> docs = {"doc", "docx", "pdf", "txt", "md",   "odt", "xls",
                                        "xlsx", "ppt", "pptx", "rtf", "csv", "epub"};
    if (images.contains(ext))
        return QObject::tr("Image");
    if (videos.contains(ext))
        return QObject::tr("Video");
    if (audios.contains(ext))
        return QObject::tr("Audio");
    if (archives.contains(ext))
        return QObject::tr("Archive");
    if (docs.contains(ext))
        return QObject::tr("Document");
    return ext.toUpper(); // e.g. "PY", "SH"
}

void FileSystemModel::sort(int column, Qt::SortOrder order) {
    m_sortColumn = column;
    m_sortOrder = order;
    beginResetModel();
    sortEntries();
    endResetModel();
}

void FileSystemModel::sortEntries() {
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);

    std::sort(m_allEntries.begin(), m_allEntries.end(), [&](const FileInfo &a, const FileInfo &b) {
        // Directories always sort before files, regardless of sort column.
        if (a.isDir() != b.isDir())
            return a.isDir();

        int cmp = 0;
        switch (m_sortColumn) {
        case SizeColumn: {
            // Use a computed folder size where we have one, so sorting by size
            // orders directories by their real footprint, not the tiny inode.
            auto effectiveSize = [this](const FileInfo &fi) {
                auto it = m_dirSizes.constFind(fi.path());
                return it != m_dirSizes.constEnd() ? it.value() : fi.size();
            };
            const qint64 sa = effectiveSize(a);
            const qint64 sb = effectiveSize(b);
            cmp = (sa < sb) ? -1 : (sa > sb ? 1 : 0);
            break;
        }
        case ModifiedColumn:
            cmp = a.modified() < b.modified() ? -1 : (a.modified() > b.modified() ? 1 : 0);
            break;
        case CreatedColumn:
            cmp = a.created() < b.created() ? -1 : (a.created() > b.created() ? 1 : 0);
            break;
        case TypeColumn:
            cmp = collator.compare(typeCategory(a), typeCategory(b));
            break;
        case ExtColumn:
            cmp = collator.compare(a.suffix(), b.suffix());
            break;
        default:
            cmp = collator.compare(a.name(), b.name());
            break;
        }
        if (cmp == 0)
            cmp = collator.compare(a.name(), b.name());
        return m_sortOrder == Qt::AscendingOrder ? cmp < 0 : cmp > 0;
    });
    applyFilter(); // keep the visible subset in sync with the sorted source
}

Qt::ItemFlags FileSystemModel::flags(const QModelIndex &index) const {
    Qt::ItemFlags f = QAbstractTableModel::flags(index);
    if (!index.isValid())
        return f;
    // Flat search-results listing is read-only: its entries live in many
    // different directories, so inline rename and drop-target semantics don't
    // apply. Dragging results out is still useful, so keep that.
    if (m_flatMode) {
        f |= Qt::ItemIsDragEnabled;
        return f;
    }
    // Inline rename is offered on the Name cell of any real entry (never
    // ".."), and on the Ext cell of real files (directories have no
    // extension to edit). Views must still call edit() explicitly (see
    // FileListView click-to-rename / MainWindow::renameCurrent): NoEditTriggers
    // is set globally so a stray keystroke never starts a rename by accident.
    if (!isParentEntry(index.row())) {
        if (index.column() == NameColumn)
            f |= Qt::ItemIsEditable;
        else if (index.column() == ExtColumn && !fileInfoAt(index.row()).isDir())
            f |= Qt::ItemIsEditable;
    }

    // Without ItemIsDragEnabled, QAbstractItemView refuses to start a drag
    // at all -- it never even calls the view's startDrag() override. ".."
    // isn't a real draggable entry, so leave it out.
    if (!isParentEntry(index.row()))
        f |= Qt::ItemIsDragEnabled;
    f |= Qt::ItemIsDropEnabled;

    return f;
}

bool FileSystemModel::setData(const QModelIndex &index, const QVariant &value, int role) {
    if (role != Qt::EditRole || isParentEntry(index.row()))
        return false;
    const int col = index.column();
    if (col != NameColumn && col != ExtColumn)
        return false;

    const FileInfo info = fileInfoAt(index.row());
    if (!info.isValid())
        return false;

    // The Name column now shows the base name and the Ext column the suffix, so
    // both edits just recombine the two halves into a full file name.
    QString newName;
    if (col == NameColumn) {
        const QString newBase = value.toString().trimmed();
        if (newBase.isEmpty())
            return false; // an empty base name would turn "photo.jpg" into ".jpg"
        // A directory (or an extension-less file) has no suffix to re-append.
        newName = info.suffix().isEmpty() ? newBase
                                          : newBase + QLatin1Char('.') + info.suffix();
    } else { // ExtColumn
        if (info.isDir())
            return false;
        const QString newExt = value.toString().trimmed();
        newName = newExt.isEmpty() ? info.baseName()
                                   : info.baseName() + QLatin1Char('.') + newExt;
    }

    if (newName.isEmpty() || newName == info.name())
        return false;

    const QString oldPath = info.path();
    QString newPath;
    switch (m_provider->rename(oldPath, newName, &newPath)) {
    case FileProvider::RenameResult::AlreadyExists:
        emit renameFailed(tr("%1 already exists").arg(newName));
        return false;
    case FileProvider::RenameResult::Failed:
        emit renameFailed(tr("Failed to rename %1").arg(info.name()));
        return false;
    case FileProvider::RenameResult::Ok:
        break;
    }

    emit renamed(oldPath, newPath);
    setRootPath(m_rootPath); // reload to pick up the new name/sort position
    return true;
}
