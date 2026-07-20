#pragma once

#include <QDialog>
#include <QFile>
#include <QStringList>

class QCheckBox;
class QLabel;

// Shows metadata for a file/directory (or a count for a multi-selection) and
// lets the user edit Unix permission bits (owner/group/other rwx), applying
// them to every path on accept. When several files disagree on a bit, its
// checkbox shows a partially-checked (mixed) state and that bit is left
// untouched unless the user sets it explicitly.
class PropertiesDialog : public QDialog {
    Q_OBJECT

public:
    explicit PropertiesDialog(const QString &path, QWidget *parent = nullptr);
    explicit PropertiesDialog(const QStringList &paths, QWidget *parent = nullptr);

    // Pure conversions between Qt's permission flags and a Unix octal triad
    // (e.g. 0755). Exposed static for unit testing.
    static int toOctal(QFile::Permissions perms);
    static QFile::Permissions fromOctal(int octal);

private:
    void buildUi();
    void updateOctalLabel();
    void apply();

    QStringList m_paths;
    QCheckBox *m_bits[9]; // owner rwx, group rwx, other rwx (row-major)
    QLabel *m_octalLabel;
};
