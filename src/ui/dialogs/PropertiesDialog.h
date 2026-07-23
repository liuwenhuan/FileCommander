#pragma once

#include "FramelessDialog.h"
#include "FileInfo.h"
#include <QFile>
#include <QStringList>

class QCheckBox;
class QLabel;

// Shows metadata for a file/directory (or a count for a multi-selection) and
// lets the user edit Unix permission bits (owner/group/other rwx), applying
// them to every path on accept. When several files disagree on a bit, its
// checkbox shows a partially-checked (mixed) state and that bit is left
// untouched unless the user sets it explicitly.
class PropertiesDialog : public FramelessDialog {
    Q_OBJECT

public:
    explicit PropertiesDialog(const QString &path, QWidget *parent = nullptr);
    explicit PropertiesDialog(const QStringList &paths, QWidget *parent = nullptr);
    // Single-item overload that carries pre-fetched metadata (owner/group,
    // permissions, size, ...). Use this for remote entries (SFTP, FTP, SMB, ...)
    // where probing `path` with QFileInfo would be meaningless.
    explicit PropertiesDialog(const FileInfo &info, QWidget *parent = nullptr);

    // Pure conversions between Qt's permission flags and a Unix octal triad
    // (e.g. 0755). Exposed static for unit testing.
    static int toOctal(QFile::Permissions perms);
    static QFile::Permissions fromOctal(int octal);

private:
    void buildUi();
    void updateOctalLabel();
    void apply();

    QStringList m_paths;
    FileInfo m_info;          // valid only when constructed from a FileInfo
    bool m_hasInfo = false;   // true -> read metadata from m_info, not QFileInfo
    QCheckBox *m_bits[9]; // owner rwx, group rwx, other rwx (row-major)
    QLabel *m_octalLabel;
};
