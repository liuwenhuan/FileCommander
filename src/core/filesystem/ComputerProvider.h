#pragma once

#include <QHash>
#include <QMutex>

#include "ComputerCatalog.h"
#include "FileProvider.h"

// Backend for the "Computer" listing: a single synthetic directory whose rows
// are places rather than files -- drives, the standard user folders, saved
// server bookmarks, plugged-in removable media and discovered network hosts.
//
// It is a FileProvider so the panel needs no special case to show it: the
// existing model, columns, sorting, icon view, tabs and navigation history all
// work unchanged, exactly as they do for ArchiveProvider's synthetic in-archive
// paths. What it deliberately does NOT do is pretend its paths are real -- every
// write operation refuses and isLocalFilesystem() stays false, so nothing can
// hand a "computer://" string to QFile and end up operating on a same-named
// local file.
//
// The entry set is pushed in from the UI layer (setEntries) rather than
// enumerated here, because the live parts of it belong to objects this layer
// cannot see: RemovableDeviceMonitor and SmbHostBrowser. list() therefore never
// blocks -- it hands back the snapshot it was given.
class ComputerProvider final : public FileProvider {
public:
    // The one path this backend lists. Deliberately not a syntactically valid
    // filesystem path on either platform, so a leak into QFile/QDir fails
    // loudly instead of resolving to something real.
    static QString rootPath();
    static bool isComputerPath(const QString &path);

    // Replaces the listing. Safe to call from the GUI thread while a scan is in
    // flight; list() takes the same lock.
    void setEntries(const QVector<ComputerEntry> &entries);

    // The entry a row stands for, so an activated row can be dispatched on its
    // kind. Returns a default-constructed entry (empty target) for an unknown
    // path.
    ComputerEntry entryFor(const QString &path) const;

    QVector<FileInfo> list(const QString &path, bool showHidden) const override;
    bool isDir(const QString &path) const override;
    QString cleanPath(const QString &path) const override;
    QString parentPath(const QString &path) const override;
    bool exists(const QString &path) const override;
    RenameResult rename(const QString &path, const QString &newName, QString *newPath) override;

    bool isVirtualListing() const override { return true; }
    QString entryTypeLabel(const QString &path) const override;
    QString entrySizeText(const QString &path) const override;
    QString entryIconPath(const QString &path) const override;
    QString entrySystemIconPath(const QString &path) const override;
    int entrySortGroup(const QString &path) const override;

private:
    // Synthetic path identifying one row, e.g. "computer://server/<uuid>".
    static QString pathForEntry(const ComputerEntry &entry);

    mutable QMutex m_mutex;
    QVector<ComputerEntry> m_entries;
    QHash<QString, ComputerEntry> m_byPath;
};
