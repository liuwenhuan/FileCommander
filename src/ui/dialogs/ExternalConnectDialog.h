#pragma once

#include <QList>
#include <QRect>
#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>

// AccountDeviceInfo is held by value in a QVector member, so moc and QVector
// both need its full definition here.
#include "account/AccountClient.h"
#include "network/ConnectionStore.h"
// SmbHost is used by value in QVector<SmbHost> members and signal arguments, so
// moc needs its full definition here (a forward declaration won't compile).
#include "network/SmbHostBrowser.h"

class QListWidget;
class QListWidgetItem;
class QEvent;

class RemovableDeviceMonitor;
struct RemovableDevice;

// The external-connection picker: a floating popup panel (not a modal dialog)
// that pops up directly above the launching button and closes as soon as focus
// leaves it, so it reads as a fly-out attached to the button rather than a
// separate window. It aggregates three kinds of navigable targets under group
// headers:
//   1. Removable devices (USB sticks / external drives) from a
//      RemovableDeviceMonitor.
//   2. Saved connections (SFTP/FTP/WebDAV/SMB bookmarks) from ConnectionStore.
//   3. Network neighbourhood hosts, discovered asynchronously through a
//      SmbHostBrowser (the list is shown immediately and host rows are appended
//      as discovery reports them).
//
// The panel owns no navigation logic: activating an entry (single click)
// emits the matching signal and closes. The two monitors are injected (owned by
// MainWindow); the panel only observes them. It deletes itself on close.
//
// Typical usage from MainWindow:
//   auto *p = new ExternalConnectDialog(m_deviceMonitor, m_smbBrowser, this);
//   connect(p, &ExternalConnectDialog::openLocalPath, ...);
//   connect(p, &ExternalConnectDialog::openSavedConnection, ...);
//   connect(p, &ExternalConnectDialog::openSmbHost, ...);
//   p->popUpAbove(m_functionKeyBar->leadingButtonGlobalRect());
class ExternalConnectDialog : public QWidget {
    Q_OBJECT

public:
    ExternalConnectDialog(RemovableDeviceMonitor *devices, SmbHostBrowser *smb,
                          QWidget *parent = nullptr);

    // Sizes the panel to its content and shows it as a popup directly above
    // `anchorGlobalRect` (the launching button's global geometry), left-aligned
    // and bottom-flush so it reads as an extension of the button. Clamped to the
    // screen the anchor sits on.
    void popUpAbove(const QRect &anchorGlobalRect);

    // The account's other devices, as the last GET /v1/devices reported them.
    // Pushed in rather than fetched: the panel has no account client, and the
    // caller already keeps this list for the "Send to Device" menu. Safe to
    // call while the panel is up -- a list that arrives late just rebuilds it,
    // which is how a device coming online shows up without reopening.
    void setAccountDevices(const QVector<AccountDeviceInfo> &devices);

    // Reapplies the current chrome-icon treatment without rebuilding this popup,
    // preserving its rows, focus, scroll position, and anchored geometry.
    void refreshThemeIcons();

signals:
    // A removable device (already mounted, or mounted on demand by the panel):
    // its local mount point, ready to hand to a panel's navigateTo().
    void openLocalPath(const QString &path);
    // A saved bookmark; the caller reconnects it through its provider.
    void openSavedConnection(const SavedConnection &conn);
    // A network-neighbourhood host name; the caller assembles smb://host.
    void openSmbHost(const QString &hostName);
    // One of the account's own devices; the caller opens a tab on its shared
    // folders.
    void openAccountDevice(const QString &deviceId, const QString &name);
    // The manager button next to the "Saved Connections" header: open the
    // connection manager (add/edit/delete saved bookmarks; manual connect,
    // including SMB, lives in its form too).
    void openConnectionManager();

protected:
    void changeEvent(QEvent *event) override;

private slots:
    void onHostsDiscovered(const QVector<SmbHost> &hosts);
    void onItemActivated(QListWidgetItem *item);

private:
    // Kind of a selectable row, stored in Qt::UserRole.
    enum Kind { KindDevice = 1, KindSaved, KindHost, KindAccountDevice };

    // Repopulates the whole list from the current device / bookmark / host
    // state and re-fits the panel size to the content.
    void rebuild();
    // A right-aligned action button on a section header (icon + tooltip + click).
    struct HeaderAction {
        QString iconPath;
        QString tooltip;
        std::function<void()> onClick;
        QString text; // optional label on the button (e.g. a "Searching…" state)
    };
    // Appends a bold, dimmed section header row, optionally carrying right-aligned
    // action buttons. All headers use the same item-widget typography path.
    void addHeader(const QString &text, const QList<HeaderAction> &actions = {});
    // Keeps every section title on the popup's inherited chrome font while
    // preserving a common bold weight and row height after live font changes.
    void syncHeaderTypography();
    // Appends a removable-device row: icon + name, plus an eject button (for a
    // mounted device) that unmounts it. The row itself stays clickable to navigate.
    void addDeviceRow(const RemovableDevice &dev);
    // Unmounts the device, then refreshes; shows an error and keeps the panel open
    // on failure.
    void ejectDevice(const QString &id);
    // Forces a fresh network-neighbourhood scan (reusing SmbHostBrowser); the
    // header's button turns into a "Searching…" state while it runs.
    void rescanNetwork();
    // Aborts a running network scan (from clicking the "Searching…" button).
    void stopNetworkScan();
    // Resizes the list (and thus the panel) to fit its rows, up to a cap.
    void fitToContents();

    RemovableDeviceMonitor *m_devices; // not owned
    SmbHostBrowser *m_smb;             // not owned

    QListWidget *m_list = nullptr;

    QVector<SavedConnection> m_saved; // parallel payload for KindSaved rows
    QVector<SmbHost> m_hosts;         // hosts accumulated from discovery
    QVector<AccountDeviceInfo> m_accountDevices; // parallel payload for KindAccountDevice rows
    bool m_discoveryDone = false;     // all sources finished (drives empty state)
    // Fly-out anchoring: the popup grows UPWARD as hosts arrive -- its bottom edge
    // stays pinned to the launching button, so newly discovered rows push the top
    // up rather than shifting everything or spilling over the button.
    int m_popupX = 0;
    int m_fixedBottomY = -1; // global Y of the pinned bottom edge (-1 = not popped up)
};
