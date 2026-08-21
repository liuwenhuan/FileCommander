#include "ExternalConnectDialog.h"

#include "RemovableDeviceMonitor.h"
#include "SmbHostBrowser.h"

#include "ThemedDialogs.h"
#include "filesystem/IconCache.h"

#include <QApplication>
#include <QEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QToolButton>
#include <QGuiApplication>
#include <QIcon>
#include <QListWidget>
#include <QScreen>
#include <QVBoxLayout>

namespace {

constexpr int kHeaderItemVerticalPadding = 8;

// Maps a saved bookmark's protocol (SavedConnection::protocol, an int mirroring
// GvfsMounter::Protocol) to a device icon. Enum order in GvfsMounter.h is:
//   0 Sftp, 1 Smb, 2 WebDav, 3 WebDavs, 4 Ftp.
QIcon themedResourceIcon(const QString &path) {
    return IconCache::instance().glyphIcon(path);
}

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
    m_list->setFrameShape(QFrame::NoFrame); // the QSS gives the list its own inset border
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto *layout = new QVBoxLayout(this);
    // Match the quick-notepad fly-out: a 6px inset so the list's inset frame
    // reads against the panel's accent border.
    layout->setContentsMargins(6, 6, 6, 6);
    layout->addWidget(m_list);

    // Single click activates (menu-like), as does double-click / Enter.
    connect(m_list, &QListWidget::itemClicked, this, &ExternalConnectDialog::onItemActivated);
    connect(m_list, &QListWidget::itemActivated, this, &ExternalConnectDialog::onItemActivated);

    // Hotplug: refresh the device rows whenever the monitor's set changes.
    if (m_devices)
        connect(m_devices, &RemovableDeviceMonitor::devicesChanged, this,
                &ExternalConnectDialog::rebuild);

    // Network neighbourhood: seed instantly from the cached results of the
    // startup (or a prior) scan, then only rescan if that cache has gone stale
    // (>4h). Host rows appended live as a running scan reports them.
    if (m_smb) {
        m_hosts = m_smb->cachedHosts();
        connect(m_smb, &SmbHostBrowser::hostsDiscovered, this,
                &ExternalConnectDialog::onHostsDiscovered);
        // When every discovery source finishes, drop the "Searching…" line for a
        // definite empty state ("no hosts found") instead of a perpetual spinner.
        connect(m_smb, &SmbHostBrowser::discoveryFinished, this, [this] {
            m_discoveryDone = true;
            rebuild();
        });
        // startDiscovery returns whether a scan is now active: if so show
        // "Searching…" until it finishes; if the fresh cache is used, we're done.
        m_discoveryDone = !m_smb->startDiscovery(false);
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

    // Pin the bottom edge to the button top: later growth (as hosts arrive) moves
    // the top up, keeping the bottom flush with the button. See fitToContents().
    m_popupX = x;
    m_fixedBottomY = anchorGlobalRect.top() + 1;

    move(x, y);
    show();
    raise();
    m_list->setFocus();
}

void ExternalConnectDialog::refreshThemeIcons() {
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem *item = m_list->item(i);
        const int kind = item->data(Qt::UserRole).toInt();
        if (kind == KindSaved) {
            const int savedIndex = item->data(Qt::UserRole + 1).toInt();
            if (savedIndex >= 0 && savedIndex < m_saved.size())
                item->setIcon(themedResourceIcon(iconForProtocol(m_saved.at(savedIndex).protocol)));
        } else if (kind == KindHost) {
            item->setIcon(themedResourceIcon(QStringLiteral(":/icons/dev-smb.svg")));
        } else if (kind == KindDevice) {
            if (QWidget *row = m_list->itemWidget(item)) {
                if (auto *iconLabel = row->findChild<QLabel *>(QStringLiteral("DeviceIcon"))) {
                    const QString path = iconLabel->property("iconPath").toString();
                    iconLabel->setPixmap(themedResourceIcon(path).pixmap(m_list->iconSize()));
                }
            }
        }
    }

    for (QToolButton *button : m_list->findChildren<QToolButton *>()) {
        const QString path = button->property("iconPath").toString();
        if (!path.isEmpty())
            button->setIcon(themedResourceIcon(path));
    }
    m_list->viewport()->update();
}

void ExternalConnectDialog::addHeader(const QString &text, const QList<HeaderAction> &actions) {
    // Every section uses the same widget path. A QListWidgetItem caches its own
    // font differently from an item widget, which previously left the actionless
    // "Removable Devices" title smaller and unable to follow live chrome-font
    // changes while the other two QLabel-backed titles did.
    auto *item = new QListWidgetItem(m_list);
    item->setFlags(Qt::NoItemFlags);
    auto *row = new QWidget(m_list);
    auto *lay = new QHBoxLayout(row);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(4);
    auto *label = new QLabel(text, row);
    label->setObjectName(QStringLiteral("ExternalConnectHeaderLabel"));
    QPalette lp = label->palette();
    lp.setColor(QPalette::WindowText, palette().color(QPalette::Disabled, QPalette::Text));
    label->setPalette(lp);
    lay->addWidget(label);
    lay->addStretch(1);
    for (const HeaderAction &a : actions) {
        auto *btn = new QToolButton(row);
        if (!a.iconPath.isEmpty()) {
            btn->setProperty("iconPath", a.iconPath);
            btn->setIcon(themedResourceIcon(a.iconPath));
        }
        if (!a.text.isEmpty()) {
            btn->setText(a.text);
            btn->setToolButtonStyle(a.iconPath.isEmpty() ? Qt::ToolButtonTextOnly
                                                         : Qt::ToolButtonTextBesideIcon);
        }
        btn->setAutoRaise(true);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setToolTip(a.tooltip);
        connect(btn, &QToolButton::clicked, this, a.onClick);
        lay->addWidget(btn);
    }
    QFont headerFont = m_list->font();
    headerFont.setBold(true);
    label->setFont(headerFont);
    item->setSizeHint(row->sizeHint());
    m_list->setItemWidget(item, row);
}

void ExternalConnectDialog::changeEvent(QEvent *event) {
    QWidget::changeEvent(event);
    if (!m_list)
        return;
    if (event->type() == QEvent::FontChange || event->type() == QEvent::ApplicationFontChange ||
        event->type() == QEvent::StyleChange) {
        syncHeaderTypography();
    }
}

void ExternalConnectDialog::syncHeaderTypography() {
    // A themed QListWidget can retain the application font resolved during QSS
    // polish instead of inheriting a later chrome-font change from this popup.
    // Keep the entire list on the popup's chrome font; content rows remain normal
    // weight while section labels derive the same font with bold enabled below.
    if (m_list->font() != font())
        m_list->setFont(font());

    QFont headerFont = font();
    headerFont.setBold(true);

    const auto labels =
        m_list->findChildren<QLabel *>(QStringLiteral("ExternalConnectHeaderLabel"));
    for (QLabel *label : labels) {
        label->setFont(headerFont);
        label->updateGeometry();
    }

    QList<QPair<QListWidgetItem *, QWidget *>> headerRows;
    int contentHeight = QFontMetrics(headerFont).height();
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem *item = m_list->item(i);
        QWidget *row = m_list->itemWidget(item);
        if (!row)
            continue;

        if (row->font() != font())
            row->setFont(font());
        if (row->layout()) {
            row->layout()->invalidate();
            row->layout()->activate();
        }
        if (item->data(Qt::UserRole).toInt() == KindDevice) {
            const auto children = row->findChildren<QWidget *>(QString(),
                                                               Qt::FindDirectChildrenOnly);
            for (QWidget *child : children) {
                if (child->font() != font())
                    child->setFont(font());
                child->updateGeometry();
            }
            if (row->layout()) {
                row->layout()->invalidate();
                row->layout()->activate();
            }
            item->setSizeHint(row->sizeHint());
            continue;
        }
        if (!row->findChild<QLabel *>(QStringLiteral("ExternalConnectHeaderLabel")))
            continue;
        contentHeight = qMax(contentHeight, row->sizeHint().height());
        headerRows.append(qMakePair(item, row));
    }

    // QSS gives each QListWidget item 4 px of top and bottom padding. An index
    // widget is laid out inside that padded content rect, so using only the
    // label/row size as the item hint clips exactly those 8 px (most visibly on
    // the first, actionless header). Keep all section rows at the same height and
    // include the style's vertical inset explicitly.
    const int itemHeight = contentHeight + kHeaderItemVerticalPadding;
    for (const auto &entry : headerRows)
        entry.first->setSizeHint(QSize(entry.second->sizeHint().width(), itemHeight));

    fitToContents();
}

void ExternalConnectDialog::addDeviceRow(const RemovableDevice &dev) {
    // iconName is a bare alias ("dev-usb"); resolve it to its resource.
    const QString iconPath = QStringLiteral(":/icons/%1.svg").arg(dev.iconName);
    const QIcon icon = themedResourceIcon(iconPath);
    auto *item = new QListWidgetItem(m_list);
    item->setData(Qt::UserRole, KindDevice);
    item->setData(Qt::UserRole + 1, dev.id);
    item->setData(Qt::UserRole + 2, dev.mountPoint);

    // Every removable device gets an eject ("safely remove") button regardless of
    // mount state -- eject unmounts it (when mounted) and powers off the drive so
    // it can be unplugged. Custom row: icon + name (transparent labels, so a click
    // on them falls through to the list and activates the row) + a right-aligned
    // eject button.
    auto *row = new QWidget(m_list);
    auto *lay = new QHBoxLayout(row);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(6);
    auto *iconLabel = new QLabel(row);
    iconLabel->setObjectName(QStringLiteral("DeviceIcon"));
    iconLabel->setProperty("iconPath", iconPath);
    iconLabel->setPixmap(icon.pixmap(m_list->iconSize()));
    iconLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto *nameLabel = new QLabel(dev.name, row);
    nameLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    lay->addWidget(iconLabel);
    lay->addWidget(nameLabel);
    lay->addStretch(1);
    auto *eject = new QToolButton(row);
    const QString ejectIconPath = QStringLiteral(":/icons/eject.svg");
    eject->setProperty("iconPath", ejectIconPath);
    eject->setIcon(themedResourceIcon(ejectIconPath));
    eject->setAutoRaise(true);
    eject->setCursor(Qt::PointingHandCursor);
    eject->setToolTip(tr("Eject (safely remove)"));
    connect(eject, &QToolButton::clicked, this, [this, id = dev.id] { ejectDevice(id); });
    lay->addWidget(eject);
    item->setSizeHint(row->sizeHint());
    m_list->setItemWidget(item, row);
}

void ExternalConnectDialog::ejectDevice(const QString &id) {
    if (!m_devices)
        return;
    QString error;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = m_devices->eject(id, &error);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        ttc::critical(this, tr("Eject Failed"),
                      tr("Cannot eject this device.\n\n%1").arg(error));
        return; // keep the panel open
    }
    // devicesChanged() from the monitor's refresh() already triggers rebuild(),
    // but rebuild explicitly too in case the signal is coalesced/delayed.
    rebuild();
}

void ExternalConnectDialog::rescanNetwork() {
    if (!m_smb)
        return;
    // Re-seed from the browser's cache instead of clearing: SmbHostBrowser only
    // emits hostsDiscovered for hosts NOT already cached, so a host cleared here
    // (but still cached) would never be re-reported and would simply vanish --
    // which is exactly why a rescan looked like it "found nothing". Keep the known
    // hosts visible; the forced scan appends any genuinely new ones on top.
    m_hosts = m_smb->cachedHosts();
    // startDiscovery(true) forces a scan and returns whether one is now active
    // (it always is under force, unless a prior scan is still running -- also
    // active). Drive m_discoveryDone off that so the trailing "Searching…" line
    // shows for the duration and clears on discoveryFinished.
    m_discoveryDone = !m_smb->startDiscovery(true);
    rebuild();
}

void ExternalConnectDialog::stopNetworkScan() {
    if (!m_smb)
        return;
    // Abort the scan: SmbHostBrowser emits discoveryFinished (which flips
    // m_discoveryDone and rebuilds the header back to the refresh icon). Set the
    // flag here too so the button updates immediately even if the signal is queued.
    m_smb->stopDiscovery();
    m_discoveryDone = true;
    rebuild();
}

void ExternalConnectDialog::setAccountDevices(const QVector<AccountDeviceInfo> &devices) {
    m_accountDevices = devices;
    rebuild();
}

void ExternalConnectDialog::rebuild() {
    m_list->clear();
    m_saved = ConnectionStore::loadAll();

    // 1. Removable devices. Each mounted device row carries an eject button.
    addHeader(tr("Removable Devices"));
    const QVector<RemovableDevice> devs = m_devices ? m_devices->devices()
                                                    : QVector<RemovableDevice>();
    if (devs.isEmpty()) {
        auto *none = new QListWidgetItem(tr("No removable devices"), m_list);
        none->setFlags(Qt::NoItemFlags);
    } else {
        for (const RemovableDevice &d : devs)
            addDeviceRow(d);
    }

    // 2. The account's own devices, when signed in and the list has arrived.
    // No section at all when signed out: an empty "My Devices" header would be
    // one more row explaining something the user has not asked for yet.
    if (!m_accountDevices.isEmpty()) {
        addHeader(tr("My Devices"));
        for (int i = 0; i < m_accountDevices.size(); ++i) {
            const AccountDeviceInfo &device = m_accountDevices.at(i);
            auto *item = new QListWidgetItem(
                themedResourceIcon(QStringLiteral(":/icons/computer.svg")), device.name, m_list);
            if (device.online && !device.self) {
                item->setData(Qt::UserRole, KindAccountDevice);
                item->setData(Qt::UserRole + 1, i);
                if (!device.shares.isEmpty())
                    item->setToolTip(tr("Shares: %1").arg(device.shares.join(QLatin1String(", "))));
            } else {
                // This machine, or one that is not listening: nothing to open.
                item->setFlags(Qt::NoItemFlags);
                item->setText(device.self ? tr("%1 (this device)").arg(device.name)
                                          : tr("%1 (offline)").arg(device.name));
            }
        }
    }

    // 3. Saved connections. The header carries a "connection manager" button that
    // opens the add/edit/delete dialog for saved bookmarks.
    addHeader(tr("Saved Connections"),
              {{QStringLiteral(":/icons/ext-connect.svg"), tr("Connection Manager…"),
                [this] {
                    emit openConnectionManager();
                    close();
                }}});
    if (m_saved.isEmpty()) {
        auto *none = new QListWidgetItem(tr("No saved connections"), m_list);
        none->setFlags(Qt::NoItemFlags);
    } else {
        for (int i = 0; i < m_saved.size(); ++i) {
            const SavedConnection &c = m_saved.at(i);
            const QString label = c.name.isEmpty() ? c.host : c.name;
            auto *item =
                new QListWidgetItem(themedResourceIcon(iconForProtocol(c.protocol)), label, m_list);
            item->setData(Qt::UserRole, KindSaved);
            item->setData(Qt::UserRole + 1, i);
        }
    }

    // 4. Network neighbourhood (populated as discovery reports hosts). The header
    // button doubles as the scan indicator: a refresh icon when idle (click to
    // rescan), or a "Searching…" label while a scan runs (click to abort it).
    HeaderAction netAction;
    if (m_discoveryDone) {
        netAction = {QStringLiteral(":/icons/refresh.svg"), tr("Search for network shares again"),
                     [this] { rescanNetwork(); }, QString()};
    } else {
        netAction = {QString(), tr("Searching — click to stop"), [this] { stopNetworkScan(); },
                     tr("Searching…")};
    }
    addHeader(tr("Network Neighborhood"), {netAction});
    for (const SmbHost &h : m_hosts) {
        // Show "name (ip)" when both are known, so the user sees exactly which
        // machine each row is; just the name or just the IP when only one is.
        // Connect BY the IP when known (a NetBIOS name like "DEEPIN-PC" may not
        // resolve via DNS), else by the name.
        QString label;
        if (h.name.isEmpty())
            label = h.address;
        else if (h.address.isEmpty())
            label = h.name;
        else
            label = QStringLiteral("%1 (%2)").arg(h.name, h.address);
        const QString target = h.address.isEmpty() ? h.name : h.address;
        auto *item = new QListWidgetItem(
            themedResourceIcon(QStringLiteral(":/icons/dev-smb.svg")), label, m_list);
        item->setData(Qt::UserRole, KindHost);
        item->setData(Qt::UserRole + 1, target);
    }
    // The "searching" state now lives on the header button, so the list only
    // shows a definite empty state once a completed scan turned up nothing.
    if (m_discoveryDone && m_hosts.isEmpty()) {
        auto *note = new QListWidgetItem(tr("No network hosts found"), m_list);
        note->setFlags(Qt::NoItemFlags);
    }

    syncHeaderTypography();
}

void ExternalConnectDialog::onHostsDiscovered(const QVector<SmbHost> &hosts) {
    // Sources report the same machine repeatedly and often partially, so the
    // batch is folded into the running list rather than appended; see
    // mergeDiscoveredHosts for how a name-only and an address-only sighting of
    // one host are recognised as the same entry.
    if (mergeDiscoveredHosts(m_hosts, hosts))
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

    // Once popped up, keep the bottom edge pinned to the button and grow upward:
    // reposition so bottom = m_fixedBottomY after the height change (clamped to
    // the screen top). Skipped during the initial pre-show fit (m_fixedBottomY<0).
    if (m_fixedBottomY >= 0 && isVisible()) {
        int y = m_fixedBottomY - height();
        if (QScreen *scr = QGuiApplication::screenAt(QPoint(m_popupX, m_fixedBottomY))) {
            if (y < scr->availableGeometry().top())
                y = scr->availableGeometry().top();
        }
        move(m_popupX, y);
    }
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
    case KindAccountDevice: {
        const int i = item->data(Qt::UserRole + 1).toInt();
        if (i < 0 || i >= m_accountDevices.size())
            return;
        emit openAccountDevice(m_accountDevices.at(i).id, m_accountDevices.at(i).name);
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
