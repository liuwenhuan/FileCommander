#pragma once

#include "FramelessDialog.h"
#include "FileInfo.h"
#include <QFile>
#include <QStringList>
#include <QVector>

class QCheckBox;
class QLabel;
class QVBoxLayout;
class DirectoryStatisticsTask;

// Shows native metadata for a file/directory (or a multi-selection). Local
// Windows entries expose timestamps and file attributes; local Unix entries
// expose owner/group and editable rwx bits. Directory size and file count are
// calculated asynchronously on local filesystems.
//
// Two flavours, and the difference matters:
//   * path/paths -- entries on THIS machine's filesystem. Metadata is read with
//     QFileInfo and permission edits go out through QFile::setPermissions.
//   * FileInfo(s) -- entries served by a provider (network share, archive).
//     Everything is read off the cached listing, and the permission grid is
//     read-only: the path names something on a server (or inside an archive),
//     so QFile::setPermissions would either fail or -- far worse -- silently
//     chmod a local file that happens to share the name.
class PropertiesDialog : public FramelessDialog {
    Q_OBJECT

public:
    explicit PropertiesDialog(const QString &path, QWidget *parent = nullptr);
    explicit PropertiesDialog(const QStringList &paths, QWidget *parent = nullptr);
    // Single-item overload that carries pre-fetched metadata (owner/group,
    // permissions, size, ...). Use this for remote entries (SFTP, FTP, SMB, ...)
    // where probing `path` with QFileInfo would be meaningless.
    explicit PropertiesDialog(const FileInfo &info, QWidget *parent = nullptr);
    // Same, for any number of entries: the multi-selection summary (item count
    // and total size) is computed from the cached listing rather than from a
    // local stat that would report zero for every one of them.
    explicit PropertiesDialog(const QVector<FileInfo> &infos, QWidget *parent = nullptr);

    // Pure conversions between Qt's permission flags and a Unix octal triad
    // (e.g. 0755). Exposed static for unit testing.
    static int toOctal(QFile::Permissions perms);
    static QFile::Permissions fromOctal(int octal);

    // Bytes across `infos`, counting files only -- a directory's own size is an
    // artefact of the filesystem, not of what the user selected. Exposed static
    // for unit testing.
    static qint64 totalFileSize(const QVector<FileInfo> &infos);

    // Initial state of the nine permission checkboxes (owner rwx, group rwx,
    // other rwx, row-major) across a set of permission values: all-set ->
    // Checked, all-clear -> Unchecked, disagreement -> PartiallyChecked. Always
    // returns exactly nine entries; an empty input reads as all-clear. Exposed
    // static for unit testing.
    static QVector<Qt::CheckState> permissionStates(const QVector<QFile::Permissions> &perms);

private:
    void buildUi();
    void startLocalStatistics(const QStringList &paths);
    void addPermissionSection(QVBoxLayout *layout);
    void updateOctalLabel();
    void apply();
#ifdef Q_OS_WIN
    void openWindowsProperties();
#endif
    void done(int result) override;

    QStringList m_paths;
    QVector<FileInfo> m_infos; // non-empty <-> provider-backed (see class note)
    // true -> metadata comes from m_infos and the permission grid is read-only.
    bool m_providerBacked = false;
    QCheckBox *m_bits[9] = {}; // owner rwx, group rwx, other rwx (row-major)
    QLabel *m_octalLabel = nullptr;
    QLabel *m_sizeLabel = nullptr;
    QLabel *m_containsLabel = nullptr;
    DirectoryStatisticsTask *m_statisticsTask = nullptr;
    bool m_closed = false;
};
