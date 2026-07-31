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

qint64 PropertiesDialog::totalFileSize(const QVector<FileInfo> &infos) {
    qint64 total = 0;
    for (const FileInfo &info : infos)
        if (!info.isDir())
            total += info.size();
    return total;
}

QVector<Qt::CheckState> PropertiesDialog::permissionStates(
    const QVector<QFile::Permissions> &perms) {
    QVector<Qt::CheckState> states(9, Qt::Unchecked);
    for (int i = 0; i < 9; ++i) {
        int set = 0;
        for (QFile::Permissions p : perms)
            if (p & kBits[i].flag)
                ++set;
        states[i] = set == 0 ? Qt::Unchecked
                    : set == perms.size() ? Qt::Checked
                                          : Qt::PartiallyChecked;
    }
    return states;
}

PropertiesDialog::PropertiesDialog(const QString &path, QWidget *parent)
    : PropertiesDialog(QStringList{path}, parent) {}

PropertiesDialog::PropertiesDialog(const QStringList &paths, QWidget *parent)
    : FramelessDialog(parent), m_paths(paths) {
    setModal(true);
    buildUi();
}

PropertiesDialog::PropertiesDialog(const FileInfo &info, QWidget *parent)
    : PropertiesDialog(QVector<FileInfo>{info}, parent) {}

PropertiesDialog::PropertiesDialog(const QVector<FileInfo> &infos, QWidget *parent)
    : FramelessDialog(parent), m_infos(infos), m_providerBacked(true) {
    m_paths.reserve(infos.size());
    for (const FileInfo &info : infos)
        m_paths.append(info.path());
    setModal(true);
    buildUi();
}

void PropertiesDialog::buildUi() {
    const bool single = m_paths.size() == 1;

    auto *form = new QFormLayout;
    // Value fields must be selectable so the name/location/etc. can be copied --
    // a plain QLabel is read-only AND non-selectable by default. The left-column
    // captions (added via addRow(QString, ...)) intentionally stay non-selectable.
    auto valueLabel = [this](const QString &text) {
        auto *l = new QLabel(text, this);
        l->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        l->setCursor(Qt::IBeamCursor);
        return l;
    };
    if (single && m_providerBacked) {
        // Remote / pre-fetched metadata: read everything off the FileInfo so we
        // never touch the (meaningless-for-remote) local filesystem.
        const FileInfo &entry = m_infos.first();
        const QString name = entry.name();
        setWindowTitle(tr("Properties — %1").arg(name));
        form->addRow(tr("Name:"), valueLabel(name));
        form->addRow(tr("Location:"), valueLabel(QFileInfo(entry.path()).path()));
        const QString type = entry.isSymLink() ? tr("Symbolic link")
                             : entry.isDir()   ? tr("Folder")
                                               : tr("File");
        form->addRow(tr("Type:"), valueLabel(type));
        if (!entry.isDir())
            form->addRow(tr("Size:"), valueLabel(humanSize(entry.size())));
        if (entry.modified().isValid())
            form->addRow(
                tr("Modified:"),
                valueLabel(entry.modified().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
        form->addRow(tr("Owner:"), valueLabel(ownerGroupText(entry.owner(), entry.ownerId())));
        form->addRow(tr("Group:"), valueLabel(ownerGroupText(entry.group(), entry.groupId())));
    } else if (single) {
        QFileInfo info(m_paths.first());
        const FileInfo entry(m_paths.first());
        setWindowTitle(tr("Properties — %1").arg(info.fileName()));
        form->addRow(tr("Name:"), valueLabel(info.fileName()));
        form->addRow(tr("Location:"), valueLabel(info.absolutePath()));
        const QString type = info.isSymLink() ? tr("Symbolic link")
                             : info.isDir()   ? tr("Folder")
                                              : tr("File");
        form->addRow(tr("Type:"), valueLabel(type));
        if (info.isSymLink())
            form->addRow(tr("Target:"), valueLabel(info.symLinkTarget()));
        if (!info.isDir())
            form->addRow(tr("Size:"), valueLabel(humanSize(info.size())));
        form->addRow(tr("Modified:"),
                     valueLabel(info.lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
        form->addRow(tr("Owner:"),
                     valueLabel(ownerGroupText(entry.owner(), entry.ownerId())));
        form->addRow(tr("Group:"),
                     valueLabel(ownerGroupText(entry.group(), entry.groupId())));
    } else {
        setWindowTitle(tr("Properties — %1 items").arg(m_paths.size()));
        // Provider-backed entries are sized from the cached listing. Stat-ing
        // them locally is what used to report "0 B" for a multi-selection on a
        // network tab -- the server's paths are not this machine's paths.
        qint64 total = 0;
        if (m_providerBacked) {
            total = totalFileSize(m_infos);
        } else {
            for (const QString &p : m_paths) {
                const QFileInfo fi(p);
                if (fi.isFile())
                    total += fi.size();
            }
        }
        form->addRow(tr("Selection:"), valueLabel(tr("%1 items").arg(m_paths.size())));
        form->addRow(tr("Total size:"), valueLabel(humanSize(total)));
    }

    // For each permission bit, decide the initial state across all entries:
    // all-set -> Checked, all-clear -> Unchecked, otherwise mixed (tri-state).
    QVector<QFile::Permissions> perms;
    perms.reserve(m_paths.size());
    if (m_providerBacked) {
        for (const FileInfo &info : m_infos)
            perms.append(info.permissions());
    } else {
        for (const QString &p : m_paths)
            perms.append(QFileInfo(p).permissions());
    }
    const QVector<Qt::CheckState> initial = permissionStates(perms);

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
            // Provider-backed entries only ever display their bits. Editing them
            // would mean QFile::setPermissions on a path that names something on
            // a server (or inside an archive): it fails, and where a local file
            // happens to share the name it succeeds on the WRONG file. Most
            // network protocols have no chmod to forward it to either, so this
            // says so rather than pretending.
            m_bits[i]->setEnabled(!m_providerBacked);
            connect(m_bits[i], &QCheckBox::stateChanged, this,
                    [this](int) { updateOctalLabel(); });
            grid->addWidget(m_bits[i], r + 1, c + 1);
        }
    }
    if (m_providerBacked) {
        auto *note = new QLabel(
            tr("Shown as reported by the source. These entries are not on this computer's "
               "filesystem, so their permissions cannot be changed here."),
            permsBox);
        note->setWordWrap(true);
        note->setEnabled(false);
        grid->addWidget(note, 4, 0, 1, 4);
    }

    m_octalLabel = new QLabel(this);
    updateOctalLabel();

    auto *buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    ttc::localizeStandardButtons(buttons);
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
    // Nothing to write back for provider-backed entries: the grid is read-only,
    // so OK just closes. Guarding here (and not only by disabling the boxes)
    // keeps QFile::setPermissions off a remote/in-archive path for good.
    if (m_providerBacked) {
        accept();
        return;
    }

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
