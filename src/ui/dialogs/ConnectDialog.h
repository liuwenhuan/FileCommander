#pragma once

#include "FramelessDialog.h"
#include <QString>

#include <functional>
#include <memory>

#include "FileProvider.h"
#include "network/ConnectionStore.h"

class QComboBox;
class QLineEdit;
class QSpinBox;
class QCheckBox;
class QDialogButtonBox;
class QListWidget;
class QPushButton;

// Commands > Connect to Server… — collects remote connection parameters,
// mounts the target through GvfsMounter (GVfs), and exposes the resulting local
// mount path so the caller can navigate the active panel to it as if it were a
// local directory.
//
// Typical usage from MainWindow (single line at the call site):
//   const QString local = ConnectDialog::runAndMount(this);
//   if (!local.isEmpty())
//       m_activePanel->navigateTo(local);
class ConnectDialog : public FramelessDialog {
    Q_OBJECT

public:
    explicit ConnectDialog(QWidget *parent = nullptr);

    // Local mount path (/run/user/<uid>/gvfs/...) after a successful accept().
    // Empty until the dialog has been accepted with a successful mount.
    QString mountedLocalPath() const { return m_mountedLocalPath; }

    // The URI that was mounted (useful for a later unmount). Empty on failure.
    QString mountedUri() const { return m_mountedUri; }

    // For a native connection (SFTP/FTP/WebDAV/SMB): the (as-yet UNCONNECTED)
    // provider, the connect closure to run on the session worker thread, and the
    // initial remote path to open. Null / empty for the gvfs-mounted protocols
    // (in that case use mountedLocalPath()). accept() no longer blocks on the
    // network -- the caller drives the async connect via connectNetwork().
    std::shared_ptr<FileProvider> remoteProvider() const { return m_remoteProvider; }
    std::function<bool(QString *)> connectFn() const { return m_connectFn; }
    QString remotePath() const { return m_remotePath; }
    // "user@host" for the chosen target, available right after accept() so the
    // caller can label the connecting tab before the link is established.
    QString displayLabel() const { return m_displayLabel; }
    // Reconnect descriptor (protocol/host/port/user/remotePath/anonymous/id) for
    // session persistence. Valid after a successful native accept().
    SavedConnection connectionInfo() const { return m_connInfo; }

    // Convenience: run the dialog modally and, on a successful mount, return the
    // local mount path. Returns an empty string if the user cancelled or the
    // mount failed. This is the recommended entry point for menu actions.
    static QString runAndMount(QWidget *parent = nullptr);

    // Preselects a protocol in the form (e.g. SMB), so a caller that already knows
    // the target kind can open the dialog ready to fill in. `protocol` is a
    // GvfsMounter::Protocol value.
    void selectProtocol(int protocol);

private slots:
    void onProtocolChanged(int index);
    void onAnonymousToggled(bool anonymous);
    void onSavedSelectionChanged();
    void onSaveConnection();
    void onDeleteConnection();

private:
    void accept() override; // performs the mount; keeps dialog open on failure

    // Rebuilds the saved-connections list widget from ConnectionStore.
    void reloadSavedList();
    // Populates the form fields from a bookmark (and its keyring password).
    void fillForm(const SavedConnection &conn);
    // Reads the current form into a bookmark (id left empty for a new one).
    SavedConnection currentFormAsConnection() const;

    QListWidget *m_savedList;
    QPushButton *m_saveButton;
    QPushButton *m_deleteButton;
    QVector<SavedConnection> m_saved; // parallel to m_savedList rows
    QString m_currentId;              // id of the loaded bookmark, empty if new

    QComboBox *m_protocolCombo;
    QLineEdit *m_hostEdit;
    QSpinBox *m_portSpin;
    QLineEdit *m_userEdit;
    QLineEdit *m_passwordEdit;
    QLineEdit *m_pathEdit;
    QCheckBox *m_anonymousCheck;
    QDialogButtonBox *m_buttons;

    QString m_mountedLocalPath;
    QString m_mountedUri;
    std::shared_ptr<FileProvider> m_remoteProvider; // native backend (UNCONNECTED)
    std::function<bool(QString *)> m_connectFn;      // runs connectToHost on the worker
    QString m_remotePath;
    QString m_displayLabel;                          // "user@host" for the connecting tab
    SavedConnection m_connInfo;                      // reconnect descriptor for session save
};
