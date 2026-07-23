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

#include "ThemedDialogs.h"
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

// Renders an owner/group cell. A resolved name wins (with the numeric id in
// parentheses when known); otherwise the bare id; otherwise "-" for unknown.
QString ownerGroupText(const QString &name, int id) {
    if (!name.isEmpty())
        return id >= 0 ? QStringLiteral("%1 (%2)").arg(name).arg(id) : name;
    if (id >= 0)
        return QString::number(id);
    return QStringLiteral("-");
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
    : PropertiesDialog(QStringList{path}, parent) {}

PropertiesDialog::PropertiesDialog(const QStringList &paths, QWidget *parent)
    : FramelessDialog(parent), m_paths(paths) {
    setModal(true);
    buildUi();
}

PropertiesDialog::PropertiesDialog(const FileInfo &info, QWidget *parent)
    : FramelessDialog(parent), m_paths(QStringList{info.path()}), m_info(info),
      m_hasInfo(true) {
    setModal(true);
    buildUi();
}

void PropertiesDialog::buildUi() {
    const bool single = m_paths.size() == 1;

    auto *form = new QFormLayout;
    if (single && m_hasInfo) {
        // Remote / pre-fetched metadata: read everything off the FileInfo so we
        // never touch the (meaningless-for-remote) local filesystem.
        const QString name = m_info.name();
        setWindowTitle(tr("Properties — %1").arg(name));
        form->addRow(tr("Name:"), new QLabel(name, this));
        form->addRow(tr("Location:"),
                     new QLabel(QFileInfo(m_info.path()).path(), this));
        const QString type = m_info.isSymLink() ? tr("Symbolic link")
                             : m_info.isDir()   ? tr("Folder")
                                                : tr("File");
        form->addRow(tr("Type:"), new QLabel(type, this));
        if (!m_info.isDir())
            form->addRow(tr("Size:"), new QLabel(humanSize(m_info.size()), this));
        if (m_info.modified().isValid())
            form->addRow(
                tr("Modified:"),
                new QLabel(m_info.modified().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                           this));
        form->addRow(tr("Owner:"),
                     new QLabel(ownerGroupText(m_info.owner(), m_info.ownerId()), this));
        form->addRow(tr("Group:"),
                     new QLabel(ownerGroupText(m_info.group(), m_info.groupId()), this));
    } else if (single) {
        QFileInfo info(m_paths.first());
        setWindowTitle(tr("Properties — %1").arg(info.fileName()));
        form->addRow(tr("Name:"), new QLabel(info.fileName(), this));
        form->addRow(tr("Location:"), new QLabel(info.absolutePath(), this));
        const QString type = info.isSymLink() ? tr("Symbolic link")
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
        form->addRow(tr("Owner:"),
                     new QLabel(ownerGroupText(info.owner(), static_cast<int>(info.ownerId())),
                                this));
        form->addRow(tr("Group:"),
                     new QLabel(ownerGroupText(info.group(), static_cast<int>(info.groupId())),
                                this));
    } else {
        setWindowTitle(tr("Properties — %1 items").arg(m_paths.size()));
        qint64 total = 0;
        for (const QString &p : m_paths) {
            const QFileInfo fi(p);
            if (fi.isFile())
                total += fi.size();
        }
        form->addRow(tr("Selection:"),
                     new QLabel(tr("%1 items").arg(m_paths.size()), this));
        form->addRow(tr("Total size:"), new QLabel(humanSize(total), this));
    }

    // For each permission bit, decide the initial state across all paths:
    // all-set -> Checked, all-clear -> Unchecked, otherwise mixed (tri-state).
    Qt::CheckState initial[9];
    for (int i = 0; i < 9; ++i) {
        int set = 0;
        if (m_hasInfo) {
            if (m_info.permissions() & kBits[i].flag)
                ++set;
        } else {
            for (const QString &p : m_paths)
                if (QFileInfo(p).permissions() & kBits[i].flag)
                    ++set;
        }
        initial[i] = set == 0 ? Qt::Unchecked
                     : set == m_paths.size() ? Qt::Checked
                                             : Qt::PartiallyChecked;
    }

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
            if (initial[i] == Qt::PartiallyChecked)
                m_bits[i]->setTristate(true);
            m_bits[i]->setCheckState(initial[i]);
            connect(m_bits[i], &QCheckBox::stateChanged, this,
                    [this](int) { updateOctalLabel(); });
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
    bool mixed = false;
    QFile::Permissions perms;
    for (int i = 0; i < 9; ++i) {
        if (m_bits[i]->checkState() == Qt::PartiallyChecked)
            mixed = true;
        else if (m_bits[i]->checkState() == Qt::Checked)
            perms |= kBits[i].flag;
    }
    m_octalLabel->setText(mixed ? tr("Octal: (mixed)")
                                : tr("Octal: %1").arg(toOctal(perms), 3, 8, QLatin1Char('0')));
}

void PropertiesDialog::apply() {
    QStringList failed;
    for (const QString &path : m_paths) {
        const QFile::Permissions orig = QFileInfo(path).permissions();
        QFile::Permissions perms = orig;
        for (int i = 0; i < 9; ++i) {
            const Qt::CheckState st = m_bits[i]->checkState();
            if (st == Qt::Checked)
                perms |= kBits[i].flag;
            else if (st == Qt::Unchecked)
                perms &= ~kBits[i].flag;
            // PartiallyChecked: leave this file's original bit as-is.
        }
        if (perms != orig && !QFile::setPermissions(path, perms))
            failed << QFileInfo(path).fileName();
    }

    if (!failed.isEmpty()) {
        ttc::warning(this, tr("Properties"),
                             tr("Failed to change permissions for:\n%1").arg(failed.join('\n')));
        return; // keep the dialog open so the user can retry or cancel
    }
    accept();
}
