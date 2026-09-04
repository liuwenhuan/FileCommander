#include "PropertiesDialog.h"

#include "DirectoryStatisticsTask.h"
#include "ThemedDialogs.h"

#include <QCheckBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QScreen>
#include <QSizePolicy>
#include <QVBoxLayout>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

struct Bit {
    QFile::Permission flag;
    int octal;
};

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

QString storageSize(qint64 bytes) {
    return bytes >= 0 ? humanSize(bytes) : PropertiesDialog::tr("Unavailable");
}

QString fileCountText(qint64 count) {
    return count == 1 ? PropertiesDialog::tr("1 file")
                      : PropertiesDialog::tr("%1 files").arg(count);
}

QString dateText(const QDateTime &date) {
    return date.isValid() ? date.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                          : PropertiesDialog::tr("Unavailable");
}

QString ownerGroupText(const QString &name, int id) {
    if (!name.isEmpty())
        return id >= 0 ? QStringLiteral("%1 (%2)").arg(name).arg(id) : name;
    if (id >= 0)
        return QString::number(id);
    return QStringLiteral("-");
}

QLabel *makeValueLabel(QWidget *parent, const QString &text, const char *objectName = nullptr) {
    auto *label = new QLabel(text, parent);
    if (objectName)
        label->setObjectName(QString::fromLatin1(objectName));
    label->setWordWrap(true);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    label->setCursor(Qt::IBeamCursor);
    return label;
}

#ifdef Q_OS_WIN
QString windowsAttributes(const QString &path) {
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()));
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return PropertiesDialog::tr("Unavailable");

    QStringList names;
    const struct {
        DWORD flag;
        const char *name;
    } known[] = {
        {FILE_ATTRIBUTE_READONLY, "Read-only"}, {FILE_ATTRIBUTE_HIDDEN, "Hidden"},
        {FILE_ATTRIBUTE_SYSTEM, "System"},      {FILE_ATTRIBUTE_ARCHIVE, "Archive"},
        {FILE_ATTRIBUTE_COMPRESSED, "Compressed"},
        {FILE_ATTRIBUTE_ENCRYPTED, "Encrypted"},
        {FILE_ATTRIBUTE_REPARSE_POINT, "Reparse point"},
    };
    for (const auto &attribute : known) {
        if (attributes & attribute.flag)
            names.append(PropertiesDialog::tr(attribute.name));
    }
    return names.isEmpty() ? PropertiesDialog::tr("Normal") : names.join(QStringLiteral(", "));
}
#endif

} // namespace

int PropertiesDialog::toOctal(QFile::Permissions perms) {
    int octal = 0;
    for (const Bit &bit : kBits) {
        if (perms & bit.flag)
            octal |= bit.octal;
    }
    return octal;
}

QFile::Permissions PropertiesDialog::fromOctal(int octal) {
    QFile::Permissions perms;
    for (const Bit &bit : kBits) {
        if (octal & bit.octal)
            perms |= bit.flag;
    }
    return perms;
}

qint64 PropertiesDialog::totalFileSize(const QVector<FileInfo> &infos) {
    qint64 total = 0;
    for (const FileInfo &info : infos) {
        if (!info.isDir())
            total += info.size();
    }
    return total;
}

QVector<Qt::CheckState> PropertiesDialog::permissionStates(
    const QVector<QFile::Permissions> &perms) {
    QVector<Qt::CheckState> states(9, Qt::Unchecked);
    for (int i = 0; i < 9; ++i) {
        int set = 0;
        for (QFile::Permissions permission : perms) {
            if (permission & kBits[i].flag)
                ++set;
        }
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

PropertiesDialog::PropertiesDialog(const RemovableDevice &device, QWidget *parent)
    : FramelessDialog(parent), m_device(device), m_deviceBacked(true) {
    if (!device.mountPoint.isEmpty())
        m_paths.append(device.mountPoint);
    setModal(true);
    buildUi();
}

void PropertiesDialog::buildUi() {
    const bool single = m_paths.size() == 1;
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    QStringList statisticsPaths;

    setWindowTitle(tr("Properties"));
    if (const QScreen *screen = QGuiApplication::primaryScreen()) {
        const int screenWidth = screen->availableGeometry().width();
        setMaximumWidth(qMax(320, screenWidth - 64));
    }

    const auto addRow = [this, form](const QString &caption, const QString &text,
                                     const char *objectName = nullptr) {
        QLabel *label = makeValueLabel(this, text, objectName);
        form->addRow(caption, label);
        return label;
    };

    if (m_deviceBacked) {
        addRow(tr("Name:"), m_device.name.isEmpty() ? tr("Unavailable") : m_device.name,
               "propertiesNameValue");
        const QString location = !m_device.mountPoint.isEmpty()
                                     ? m_device.mountPoint
                                     : m_device.devNode;
        addRow(tr("Location:"), location.isEmpty() ? tr("Unavailable") : location,
               "propertiesLocationValue");
        addRow(tr("Type:"), tr("Removable device"), "propertiesTypeValue");
        addRow(tr("Total capacity:"), storageSize(m_device.bytesTotal),
               "propertiesCapacityValue");
        addRow(tr("Available space:"), storageSize(m_device.bytesAvailable),
               "propertiesAvailableValue");
    } else if (single && m_providerBacked) {
        const FileInfo &entry = m_infos.first();
        addRow(tr("Name:"), entry.name(), "propertiesNameValue");
        addRow(tr("Location:"), QFileInfo(entry.path()).path(), "propertiesLocationValue");
        const QString type = entry.isSymLink() ? tr("Symbolic link")
                             : entry.isDir()   ? tr("Folder")
                                               : tr("File");
        addRow(tr("Type:"), type, "propertiesTypeValue");
        if (entry.isDir()) {
            m_sizeLabel = addRow(tr("Size:"), tr("Unavailable"), "propertiesSizeValue");
            m_containsLabel =
                addRow(tr("Contains:"), tr("Unavailable"), "propertiesContainsValue");
        } else {
            m_sizeLabel = addRow(tr("Size:"), humanSize(entry.size()), "propertiesSizeValue");
        }
        if (entry.modified().isValid())
            addRow(tr("Modified:"), dateText(entry.modified()), "propertiesModifiedValue");
        addRow(tr("Owner:"), ownerGroupText(entry.owner(), entry.ownerId()),
               "propertiesOwnerValue");
        addRow(tr("Group:"), ownerGroupText(entry.group(), entry.groupId()),
               "propertiesGroupValue");
    } else if (single) {
        const QFileInfo info(m_paths.first());
        const FileInfo entry(m_paths.first());
        addRow(tr("Name:"), info.fileName(), "propertiesNameValue");
        addRow(tr("Location:"), info.absolutePath(), "propertiesLocationValue");
        const QString type = info.isSymLink() ? tr("Symbolic link")
                             : info.isDir()   ? tr("Folder")
                                              : tr("File");
        addRow(tr("Type:"), type, "propertiesTypeValue");
        if (info.isSymLink())
            addRow(tr("Target:"), info.symLinkTarget(), "propertiesTargetValue");

        if (info.isDir() && !info.isSymLink()) {
            m_sizeLabel = addRow(tr("Size:"), tr("Calculating..."), "propertiesSizeValue");
            m_containsLabel =
                addRow(tr("Contains:"), tr("Calculating..."), "propertiesContainsValue");
            statisticsPaths.append(info.absoluteFilePath());
        } else {
            m_sizeLabel = addRow(tr("Size:"), humanSize(info.size()), "propertiesSizeValue");
        }

#ifdef Q_OS_WIN
        addRow(tr("Created:"), dateText(info.birthTime()), "propertiesCreatedValue");
        addRow(tr("Modified:"), dateText(info.lastModified()), "propertiesModifiedValue");
        addRow(tr("Accessed:"), dateText(info.lastRead()), "propertiesAccessedValue");
        addRow(tr("Attributes:"), windowsAttributes(info.absoluteFilePath()),
               "propertiesAttributesValue");
#else
        addRow(tr("Modified:"), dateText(info.lastModified()), "propertiesModifiedValue");
        addRow(tr("Owner:"), ownerGroupText(entry.owner(), entry.ownerId()),
               "propertiesOwnerValue");
        addRow(tr("Group:"), ownerGroupText(entry.group(), entry.groupId()),
               "propertiesGroupValue");
#endif
    } else {
        addRow(tr("Selection:"), tr("%1 items").arg(m_paths.size()),
               "propertiesSelectionValue");

        if (m_providerBacked) {
            bool hasDirectory = false;
            for (const FileInfo &info : m_infos)
                hasDirectory = hasDirectory || info.isDir();
            m_sizeLabel = addRow(tr("Total size:"),
                                 hasDirectory ? tr("Unavailable")
                                              : humanSize(totalFileSize(m_infos)),
                                 "propertiesSizeValue");
            if (hasDirectory) {
                m_containsLabel =
                    addRow(tr("Contains:"), tr("Unavailable"), "propertiesContainsValue");
            }
        } else {
            bool hasDirectory = false;
            qint64 immediateBytes = 0;
            for (const QString &path : m_paths) {
                const QFileInfo info(path);
                hasDirectory = hasDirectory || (info.isDir() && !info.isSymLink());
                if (!info.isDir() || info.isSymLink())
                    immediateBytes += info.size();
            }
            if (hasDirectory) {
                m_sizeLabel =
                    addRow(tr("Total size:"), tr("Calculating..."), "propertiesSizeValue");
                m_containsLabel =
                    addRow(tr("Contains:"), tr("Calculating..."), "propertiesContainsValue");
                statisticsPaths = m_paths;
            } else {
                m_sizeLabel = addRow(tr("Total size:"), humanSize(immediateBytes),
                                     "propertiesSizeValue");
            }
        }
    }

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    addPermissionSection(layout);

    auto *buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    ttc::localizeStandardButtons(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &PropertiesDialog::apply);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

#ifdef Q_OS_WIN
    auto *buttonLayout = new QHBoxLayout;
    auto *allProperties = new QPushButton(tr("All Properties"), this);
    allProperties->setObjectName(QStringLiteral("propertiesAllButton"));
    const bool canShowWindowsProperties =
        !m_providerBacked && single && QFileInfo(m_paths.first()).exists();
    allProperties->setEnabled(canShowWindowsProperties);
    allProperties->setToolTip(tr("Available for one local file or folder."));
    connect(allProperties, &QPushButton::clicked, this, &PropertiesDialog::openWindowsProperties);
    buttonLayout->addWidget(allProperties);
    buttonLayout->addStretch();
    buttonLayout->addWidget(buttons);
    layout->addLayout(buttonLayout);
#else
    layout->addWidget(buttons);
#endif

    if (!statisticsPaths.isEmpty())
        startLocalStatistics(statisticsPaths);
}

void PropertiesDialog::startLocalStatistics(const QStringList &paths) {
    m_statisticsTask = new DirectoryStatisticsTask(paths, this);
    connect(m_statisticsTask, &DirectoryStatisticsTask::finished, this,
            [this](qint64 bytes, qint64 fileCount, bool cancelled) {
                if (cancelled || m_closed)
                    return;
                if (m_sizeLabel)
                    m_sizeLabel->setText(humanSize(bytes));
                if (m_containsLabel)
                    m_containsLabel->setText(fileCountText(fileCount));
            });
    m_statisticsTask->start();
}

void PropertiesDialog::addPermissionSection(QVBoxLayout *layout) {
    if (m_deviceBacked)
        return;
#ifdef Q_OS_WIN
    // Local Windows entries expose native attributes instead of Unix mode bits.
    // Provider metadata remains visible, read-only, because it describes the
    // remote source rather than the Windows filesystem.
    if (!m_providerBacked)
        return;
#endif

    QVector<QFile::Permissions> permissions;
    permissions.reserve(m_paths.size());
    if (m_providerBacked) {
        for (const FileInfo &info : m_infos)
            permissions.append(info.permissions());
    } else {
        for (const QString &path : m_paths)
            permissions.append(QFileInfo(path).permissions());
    }
    const QVector<Qt::CheckState> initial = permissionStates(permissions);

    auto *box = new QGroupBox(tr("Permissions"), this);
    box->setObjectName(QStringLiteral("propertiesPermissionsGroup"));
    auto *grid = new QGridLayout(box);
    const QString rowLabels[3] = {tr("Owner"), tr("Group"), tr("Others")};
    const QString columnLabels[3] = {tr("Read"), tr("Write"), tr("Execute")};
    for (int column = 0; column < 3; ++column)
        grid->addWidget(new QLabel(columnLabels[column], box), 0, column + 1);
    for (int row = 0; row < 3; ++row) {
        grid->addWidget(new QLabel(rowLabels[row], box), row + 1, 0);
        for (int column = 0; column < 3; ++column) {
            const int index = row * 3 + column;
            m_bits[index] = new QCheckBox(box);
            if (initial[index] == Qt::PartiallyChecked)
                m_bits[index]->setTristate(true);
            m_bits[index]->setCheckState(initial[index]);
            m_bits[index]->setEnabled(!m_providerBacked);
            connect(m_bits[index], &QCheckBox::stateChanged, this,
                    [this](int) { updateOctalLabel(); });
            grid->addWidget(m_bits[index], row + 1, column + 1);
        }
    }
    if (m_providerBacked) {
        auto *note = new QLabel(
            tr("Shown as reported by the source. These entries are not on this computer's "
               "filesystem, so their permissions cannot be changed here."),
            box);
        note->setWordWrap(true);
        note->setEnabled(false);
        grid->addWidget(note, 4, 0, 1, 4);
    }

    m_octalLabel = new QLabel(this);
    m_octalLabel->setObjectName(QStringLiteral("propertiesOctalValue"));
    updateOctalLabel();
    layout->addWidget(box);
    layout->addWidget(m_octalLabel);
}

void PropertiesDialog::updateOctalLabel() {
    if (!m_octalLabel)
        return;
    bool mixed = false;
    QFile::Permissions permissions;
    for (int i = 0; i < 9; ++i) {
        if (m_bits[i]->checkState() == Qt::PartiallyChecked)
            mixed = true;
        else if (m_bits[i]->checkState() == Qt::Checked)
            permissions |= kBits[i].flag;
    }
    m_octalLabel->setText(
        mixed ? tr("Octal: (mixed)")
              : tr("Octal: %1").arg(toOctal(permissions), 3, 8, QLatin1Char('0')));
}

void PropertiesDialog::apply() {
    if (m_providerBacked || m_deviceBacked) {
        accept();
        return;
    }

#ifdef Q_OS_WIN
    accept();
#else
    QStringList failed;
    for (const QString &path : m_paths) {
        const QFile::Permissions original = QFileInfo(path).permissions();
        QFile::Permissions permissions = original;
        for (int i = 0; i < 9; ++i) {
            const Qt::CheckState state = m_bits[i]->checkState();
            if (state == Qt::Checked)
                permissions |= kBits[i].flag;
            else if (state == Qt::Unchecked)
                permissions &= ~kBits[i].flag;
        }
        if (permissions != original && !QFile::setPermissions(path, permissions))
            failed << QFileInfo(path).fileName();
    }
    if (!failed.isEmpty()) {
        ttc::warning(this, tr("Properties"),
                     tr("Failed to change permissions for:\n%1").arg(failed.join('\n')));
        return;
    }
    accept();
#endif
}

#ifdef Q_OS_WIN
void PropertiesDialog::openWindowsProperties() {
    if (m_providerBacked || m_paths.size() != 1 || !QFileInfo(m_paths.first()).exists())
        return;

    const QString path = m_paths.first();
    SHELLEXECUTEINFOW executeInfo{};
    executeInfo.cbSize = sizeof(executeInfo);
    executeInfo.fMask = SEE_MASK_INVOKEIDLIST | SEE_MASK_ASYNCOK;
    executeInfo.hwnd = reinterpret_cast<HWND>(winId());
    executeInfo.lpVerb = L"properties";
    executeInfo.lpFile = reinterpret_cast<LPCWSTR>(path.utf16());
    executeInfo.nShow = SW_SHOWNORMAL;
    ShellExecuteExW(&executeInfo);
}
#endif

void PropertiesDialog::done(int result) {
    m_closed = true;
    if (m_statisticsTask)
        m_statisticsTask->cancel();
    FramelessDialog::done(result);
}
