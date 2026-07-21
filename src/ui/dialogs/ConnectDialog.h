#pragma once

#include <QDialog>
#include <QString>

class QComboBox;
class QLineEdit;
class QSpinBox;
class QCheckBox;
class QDialogButtonBox;

// Commands > Connect to Server… — collects remote connection parameters,
// mounts the target through GvfsMounter (GVfs), and exposes the resulting local
// mount path so the caller can navigate the active panel to it as if it were a
// local directory.
//
// Typical usage from MainWindow (single line at the call site):
//   const QString local = ConnectDialog::runAndMount(this);
//   if (!local.isEmpty())
//       m_activePanel->navigateTo(local);
class ConnectDialog : public QDialog {
    Q_OBJECT

public:
    explicit ConnectDialog(QWidget *parent = nullptr);

    // Local mount path (/run/user/<uid>/gvfs/...) after a successful accept().
    // Empty until the dialog has been accepted with a successful mount.
    QString mountedLocalPath() const { return m_mountedLocalPath; }

    // The URI that was mounted (useful for a later unmount). Empty on failure.
    QString mountedUri() const { return m_mountedUri; }

    // Convenience: run the dialog modally and, on a successful mount, return the
    // local mount path. Returns an empty string if the user cancelled or the
    // mount failed. This is the recommended entry point for menu actions.
    static QString runAndMount(QWidget *parent = nullptr);

private slots:
    void onProtocolChanged(int index);
    void onAnonymousToggled(bool anonymous);

private:
    void accept() override; // performs the mount; keeps dialog open on failure

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
};
