#include "ExternalConnectDialog.h"

#include "RemovableDeviceMonitor.h"
#include "SmbHostBrowser.h"

#include "ThemedDialogs.h"

#include <QApplication>
#include <QFont>
#include <QGuiApplication>
#include <QIcon>
#include <QListWidget>
#include <QScreen>
#include <QVBoxLayout>

namespace {

// Maps a saved bookmark's protocol (SavedConnection::protocol, an int mirroring
// GvfsMounter::Protocol) to a device icon. Enum order in GvfsMounter.h is:
//   0 Sftp, 1 Smb, 2 WebDav, 3 WebDavs, 4 Ftp.
QString iconForProtocol(int protocol) {
    switch (protocol) {
    case 0: // Sftp
        return QStringLiteral(":/icons/dev-sftp.svg");
    case 1: // Smb
        return QStringLiteral(":/icons/dev-smb.svg");
    case 2: // WebDav
    case 3: // WebDavs
        return QStringLiteral(":/icons/dev-webdav.svg");
    case 4: // Ftp
        return QStringLiteral(":/icons/dev-ftp.svg");
    default:
        return QStringLiteral(":/icons/dev-smb.svg");
    }
}

} // namespace

ExternalConnectDialog::ExternalConnectDialog(RemovableDeviceMonitor *devices,
                                             SmbHostBrowser *smb, QWidget *parent)
    // Qt::Popup: a top-level fly-out that grabs input and closes the moment the
    // user clicks outside it -- exactly the "attached to the button" behaviour.
    : QWidget(parent, Qt::Popup), m_devices(devices), m_smb(smb) {
    // Styled by #ExternalConnectPanel in the theme QSS (rounded top, accent
    // border, bottom-flush with the launching button).
    setObjectName(QStringLiteral("ExternalConnectPanel"));
    setAttribute(Qt::WA_DeleteOnClose);
    // A bare QWidget only honours QSS border / border-radius on its #objectName
    // when it paints a styled background, so the accent frame renders on every
    // edge (not just the list's own rows).
    setAttribute(Qt::WA_StyledBackground, true);

    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("ExternalConnectList"));
    m_list->setIconSize(QSize(20, 20));
    m_list->setUniformItemSizes(false);
    m_list->setMinimumWidth(340);
    m_list->setFrameShape(QFrame::NoFrame); // the panel supplies the border
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto *layout = new QVBoxLayout(this);
    // A slim uniform inset so the list sits just inside the panel's border.
    layout->setContentsMargins(1, 1, 1, 1);
    layout->addWidget(m_list);

    // Single click activates (menu-like), as does double-click / Enter.
    connect(m_list, &QListWidget::itemClicked, this, &ExternalConnectDialog::onItemActivated);
    connect(m_list, &QListWidget::itemActivated, this, &ExternalConnectDialog::onItemActivated);

    // Hotplug: refresh the device rows whenever the monitor's set changes.
    if (m_devices)
        connect(m_devices, &RemovableDeviceMonitor::devicesChanged, this,
                &ExternalConnectDialog::rebuild);

    // Network neighbourhood discovery is asynchronous: kick it off and append
    // host rows as they arrive. The first two sections render immediately.
    if (m_smb) {
        connect(m_smb, &SmbHostBrowser::hostsDiscovered, this,
                &ExternalConnectDialog::onHostsDiscovered);
        m_smb->startDiscovery();
    }

    rebuild();
}

void ExternalConnectDialog::popUpAbove(const QRect &anchorGlobalRect) {
    fitToContents(); // establishes the panel's size

    int x = anchorGlobalRect.left();
    // Bottom edge overlaps the button's top edge by 1px so their borders merge
    // and the panel reads as a fly-out growing out of the button.
    int y = anchorGlobalRect.top() - height() + 1;

    // Keep the panel on the screen the button lives on.
    if (QScreen *scr = QGuiApplication::screenAt(anchorGlobalRect.center())) {
        const QRect avail = scr->availableGeometry();
        if (x + width() > avail.right())
            x = avail.right() - width();
        if (x < avail.left())
            x = avail.left();
        if (y < avail.top())
            y = avail.top();
    }

    move(x, y);
    show();
    raise();
    m_list->setFocus();
}

void ExternalConnectDialog::addHeader(const QString &text) {
    auto *item = new QListWidgetItem(text, m_list);
    // Non-interactive section label: no selection, no activation.
    item->setFlags(Qt::NoItemFlags);
    QFont f = item->font();
    f.setBold(true);
    item->setFont(f);
    // Follow the theme: dim the header relative to normal text.
    item->setForeground(palette().color(QPalette::Disabled, QPalette::Text));
}

void ExternalConnectDialog::rebuild() {
    m_list->clear();
    m_saved = ConnectionStore::loadAll();

    // 1. Removable devices.
    addHeader(tr("Removable Devices"));
    const QVector<RemovableDevice> devs = m_devices ? m_devices->devices()
                                                    : QVector<RemovableDevice>();
    if (devs.isEmpty()) {
        auto *none = new QListWidgetItem(tr("No removable devices"), m_list);
        none->setFlags(Qt::NoItemFlags);
    } else {
        for (const RemovableDevice &d : devs) {
            // iconName is a bare alias ("dev-usb"); resolve it to its resource.
            const QIcon icon(QStringLiteral(":/icons/%1.svg").arg(d.iconName));
            auto *item = new QListWidgetItem(icon, d.name, m_list);
            item->setData(Qt::UserRole, KindDevice);
            item->setData(Qt::UserRole + 1, d.id);
            item->setData(Qt::UserRole + 2, d.mountPoint);
        }
    }

    // 2. Saved connections.
    addHeader(tr("Saved Connections"));
    if (m_saved.isEmpty()) {
        auto *none = new QListWidgetItem(tr("No saved connections"), m_list);
        none->setFlags(Qt::NoItemFlags);
    } else {
        for (int i = 0; i < m_saved.size(); ++i) {
            const SavedConnection &c = m_saved.at(i);
            const QString label = c.name.isEmpty() ? c.host : c.name;
            auto *item =
                new QListWidgetItem(QIcon(iconForProtocol(c.protocol)), label, m_list);
            item->setData(Qt::UserRole, KindSaved);
            item->setData(Qt::UserRole + 1, i);
        }
    }

    // 3. Network neighbourhood (populated as discovery reports hosts).
    addHeader(tr("Network Neighborhood"));
    if (m_hosts.isEmpty()) {
        auto *searching = new QListWidgetItem(tr("Searching…"), m_list);
        searching->setFlags(Qt::NoItemFlags);
    } else {
        for (const SmbHost &h : m_hosts) {
            const QString label = h.name.isEmpty() ? h.address : h.name;
            auto *item = new QListWidgetItem(QIcon(QStringLiteral(":/icons/dev-smb.svg")),
                                             label, m_list);
            item->setData(Qt::UserRole, KindHost);
            item->setData(Qt::UserRole + 1, h.name.isEmpty() ? h.address : h.name);
        }
    }

    fitToContents();
}

void ExternalConnectDialog::onHostsDiscovered(const QVector<SmbHost> &hosts) {
    // Merge new hosts, skipping ones we already show (by name+address).
    bool added = false;
    for (const SmbHost &h : hosts) {
        bool known = false;
        for (const SmbHost &existing : m_hosts) {
            if (existing.name == h.name && existing.address == h.address) {
                known = true;
                break;
            }
        }
        if (!known) {
            m_hosts.append(h);
            added = true;
        }
    }
    if (added)
        rebuild();
}

void ExternalConnectDialog::fitToContents() {
    int h = 2 * m_list->frameWidth();
    for (int i = 0; i < m_list->count(); ++i)
        h += m_list->sizeHintForRow(i);
    // Cap the height so a long neighbourhood list scrolls instead of growing
    // past a comfortable size; short lists shrink to fit.
    const int maxHeight = 480;
    m_list->setFixedHeight(qMin(h + 2, maxHeight));
    adjustSize();
}

void ExternalConnectDialog::onItemActivated(QListWidgetItem *item) {
    if (!item)
        return;
    const QVariant kindVar = item->data(Qt::UserRole);
    if (!kindVar.isValid())
        return; // header or placeholder row

    switch (kindVar.toInt()) {
    case KindDevice: {
        const QString id = item->data(Qt::UserRole + 1).toString();
        QString mountPoint = item->data(Qt::UserRole + 2).toString();
        if (mountPoint.isEmpty() && m_devices) {
            // Mount on demand; this can block briefly.
            QString error;
            QApplication::setOverrideCursor(Qt::WaitCursor);
            mountPoint = m_devices->ensureMounted(id, &error);
            QApplication::restoreOverrideCursor();
            if (mountPoint.isEmpty()) {
                ttc::critical(this, tr("Mount Failed"),
                              tr("Could not mount the device.\n\n%1").arg(error));
                return; // keep the panel open
            }
        }
        if (mountPoint.isEmpty())
            return;
        emit openLocalPath(mountPoint);
        close();
        break;
    }
    case KindSaved: {
        const int i = item->data(Qt::UserRole + 1).toInt();
        if (i < 0 || i >= m_saved.size())
            return;
        emit openSavedConnection(m_saved.at(i));
        close();
        break;
    }
    case KindHost: {
        const QString host = item->data(Qt::UserRole + 1).toString();
        if (host.isEmpty())
            return;
        emit openSmbHost(host);
        close();
        break;
    }
    default:
        break;
    }
}
