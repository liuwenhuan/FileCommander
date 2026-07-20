#include "PropertiesDialog.h"

#include <QCheckBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QVBoxLayout>

namespace {

struct Bit {
    QFile::Permission flag;
    int octal;
};

// Row-major: owner rwx, group rwx, other rwx -- index matches m_bits[].
const Bit kBits[9] = {
    {QFile::ReadOwner, 0400}, {QFile::WriteOwner, 0200}, {QFile::ExeOwner, 0100},
    {QFile::ReadGroup, 0040}, {QFile::WriteGroup, 0020}, {QFile::ExeGroup, 0010},
    {QFile::ReadOther, 0004}, {QFile::WriteOther, 0002}, {QFile::ExeOther, 0001},
};

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
    return QStringLiteral("%1 %2 (%3 bytes)")
        .arg(size, 0, 'f', 1)
        .arg(units[unit])
        .arg(QLocale().toString(bytes));
}

} // namespace

int PropertiesDialog::toOctal(QFile::Permissions perms) {
    int octal = 0;
    for (const Bit &b : kBits)
        if (perms & b.flag)
            octal |= b.octal;
    return octal;
}

QFile::Permissions PropertiesDialog::fromOctal(int octal) {
    QFile::Permissions perms;
    for (const Bit &b : kBits)
        if (octal & b.octal)
            perms |= b.flag;
    return perms;
}

PropertiesDialog::PropertiesDialog(const QString &path, QWidget *parent)
    : QDialog(parent), m_path(path) {
    QFileInfo info(path);
    m_originalPerms = info.permissions();

    setWindowTitle(tr("Properties — %1").arg(info.fileName()));
    setModal(true);

    auto *form = new QFormLayout;
    form->addRow(tr("Name:"), new QLabel(info.fileName(), this));
    form->addRow(tr("Location:"), new QLabel(info.absolutePath(), this));

    QString type = info.isSymLink() ? tr("Symbolic link")
                   : info.isDir()   ? tr("Folder")
                                    : tr("File");
    form->addRow(tr("Type:"), new QLabel(type, this));
    if (info.isSymLink())
        form->addRow(tr("Target:"), new QLabel(info.symLinkTarget(), this));
    if (!info.isDir())
        form->addRow(tr("Size:"), new QLabel(humanSize(info.size()), this));
    form->addRow(tr("Modified:"),
                 new QLabel(info.lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                            this));
    form->addRow(tr("Owner:"), new QLabel(info.owner(), this));
    form->addRow(tr("Group:"), new QLabel(info.group(), this));

    auto *permsBox = new QGroupBox(tr("Permissions"), this);
    auto *grid = new QGridLayout(permsBox);
    const QString rowLabels[3] = {tr("Owner"), tr("Group"), tr("Others")};
    const QString colLabels[3] = {tr("Read"), tr("Write"), tr("Execute")};
    for (int c = 0; c < 3; ++c)
        grid->addWidget(new QLabel(colLabels[c], permsBox), 0, c + 1);
    for (int r = 0; r < 3; ++r) {
        grid->addWidget(new QLabel(rowLabels[r], permsBox), r + 1, 0);
        for (int c = 0; c < 3; ++c) {
            const int i = r * 3 + c;
            m_bits[i] = new QCheckBox(permsBox);
            m_bits[i]->setChecked(m_originalPerms & kBits[i].flag);
            connect(m_bits[i], &QCheckBox::toggled, this, &PropertiesDialog::updateOctalLabel);
            grid->addWidget(m_bits[i], r + 1, c + 1);
        }
    }

    m_octalLabel = new QLabel(this);
    updateOctalLabel();

    auto *buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &PropertiesDialog::apply);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(permsBox);
    layout->addWidget(m_octalLabel);
    layout->addWidget(buttons);
}

void PropertiesDialog::updateOctalLabel() {
    QFile::Permissions perms;
    for (int i = 0; i < 9; ++i)
        if (m_bits[i]->isChecked())
            perms |= kBits[i].flag;
    m_octalLabel->setText(
        tr("Octal: %1").arg(toOctal(perms), 3, 8, QLatin1Char('0')));
}

void PropertiesDialog::apply() {
    QFile::Permissions perms;
    for (int i = 0; i < 9; ++i)
        if (m_bits[i]->isChecked())
            perms |= kBits[i].flag;

    if (perms != m_originalPerms && !QFile::setPermissions(m_path, perms)) {
        QMessageBox::warning(this, tr("Properties"),
                             tr("Failed to change permissions for %1").arg(m_path));
        return; // keep the dialog open so the user can retry or cancel
    }
    accept();
}
