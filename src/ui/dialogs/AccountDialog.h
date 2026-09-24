#pragma once

#include "FramelessDialog.h"

class AccountClient;
class Settings;

class QCheckBox;
class QScrollArea;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QRadioButton;
class QStackedWidget;

// Sign in to the FileCommander account that device-to-device transfer runs on
// (see server/README.md). Two faces of the same dialog: the sign-in form while
// signed out, the account and its devices while signed in.
//
// The dialog owns no session state. It drives the AccountClient it is handed --
// which outlives it -- and writes only non-secret bookkeeping (email, device
// id and endpoint choice) to Settings. Refresh tokens are the client's business
// and never reach the INI.
class AccountDialog : public FramelessDialog {
    Q_OBJECT

public:
    AccountDialog(AccountClient &client, Settings &settings, QWidget *parent = nullptr);
    QSize sizeHint() const override;

signals:
    // A device row was activated. The dialog is modal, so it accepts itself and
    // leaves the actual tab-opening to MainWindow -- browsing a peer behind a
    // modal dialog would be a window the user cannot get past.
    void openDevice(const QString &deviceId, const QString &name);

private:
    // Swaps between the sign-in form and the signed-in page, and refreshes the
    // device list when there is a session to list it from.
    void showCurrentState();
    void saveSharedFolders();
    void setBusy(bool busy);
    void setSignedOutStatus(const QString &message);
    void updateWindowSize();
    void reportError(const QString &error);
    void updateServerControls();
    bool applyServerSelection();

    AccountClient &m_client;
    Settings &m_settings;

    QStackedWidget *m_pages = nullptr;
    QScrollArea *m_loginScroll = nullptr;
    QRadioButton *m_officialServer = nullptr;
    QRadioButton *m_customServer = nullptr;
    QLineEdit *m_customServerUrl;
    QLineEdit *m_email;
    QLineEdit *m_password;
    QLineEdit *m_deviceName;
    QPushButton *m_signIn;
    QPushButton *m_registerButton;
    QLabel *m_status = nullptr;
    QLabel *m_accountStatus;
    QLabel *m_accountLabel;
    QListWidget *m_devices;
    QCheckBox *m_shareEnabled;
    QListWidget *m_sharedFolders;
};
