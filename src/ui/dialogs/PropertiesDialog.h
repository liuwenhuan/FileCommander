#pragma once

#include <QDialog>
#include <QFile>

class QCheckBox;
class QLabel;

// Shows metadata for a single file/directory and lets the user edit its Unix
// permission bits (owner/group/other rwx). Applies changes with
// QFile::setPermissions on accept.
class PropertiesDialog : public QDialog {
    Q_OBJECT

public:
    explicit PropertiesDialog(const QString &path, QWidget *parent = nullptr);

    // Pure conversions between Qt's permission flags and a Unix octal triad
    // (e.g. 0755). Exposed static for unit testing.
    static int toOctal(QFile::Permissions perms);
    static QFile::Permissions fromOctal(int octal);

private:
    void updateOctalLabel();
    void apply();

    QString m_path;
    QFile::Permissions m_originalPerms;
    QCheckBox *m_bits[9]; // owner rwx, group rwx, other rwx (row-major)
    QLabel *m_octalLabel;
};
