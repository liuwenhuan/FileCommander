#include "AccountDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHostInfo>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QStackedWidget>
#include <QUrl>
#include <QVBoxLayout>

#include "account/AccountClient.h"
#include "config/Settings.h"

namespace {

QString normalizedServerUrl(QString value) {
    value = value.trimmed();
    while (value.endsWith(QLatin1Char('/')))
        value.chop(1);
    return value;
}

bool isValidCustomServerUrl(const QString &value) {
    const QUrl url(value, QUrl::StrictMode);
    const QString scheme = url.scheme().toLower();
    return url.isValid() && (scheme == QLatin1String("http") || scheme == QLatin1String("https")) &&
           !url.host().isEmpty() && url.userInfo().isEmpty() && url.query().isEmpty() &&
           url.fragment().isEmpty();
}

} // namespace

AccountDialog::AccountDialog(AccountClient &client, Settings &settings, QWidget *parent)
    : FramelessDialog(parent), m_client(client), m_settings(settings) {
    setWindowTitle(tr("FileCommander Account"));
    setModal(true);
    resize(500, 430);

    m_pages = new QStackedWidget(this);

    // Page 0: signed out.
    auto *form = new QWidget(m_pages);
    auto *fields = new QFormLayout(form);

    auto *serverChoices = new QWidget(form);
    auto *serverLayout = new QVBoxLayout(serverChoices);
    serverLayout->setContentsMargins(0, 0, 0, 0);
    m_officialServer = new QRadioButton(tr("Official server"), serverChoices);
    m_officialServer->setObjectName(QStringLiteral("OfficialServerRadio"));
    m_customServer = new QRadioButton(tr("Custom server"), serverChoices);
    m_customServer->setObjectName(QStringLiteral("CustomServerRadio"));
    m_customServerUrl = new QLineEdit(serverChoices);
    m_customServerUrl->setObjectName(QStringLiteral("CustomServerUrl"));
    m_customServerUrl->setPlaceholderText(tr("Server URL"));
    m_customServerUrl->setText(m_settings.accountCustomServerUrl());
    auto *customRow = new QHBoxLayout;
    customRow->setContentsMargins(0, 0, 0, 0);
    customRow->addWidget(m_customServer);
    customRow->addWidget(m_customServerUrl, 1);
    serverLayout->addWidget(m_officialServer);
    serverLayout->addLayout(customRow);
    const bool official = m_settings.accountUsesOfficialServer();
    m_officialServer->setChecked(official);
    m_customServer->setChecked(!official);

    m_email = new QLineEdit(form);
    m_email->setObjectName(QStringLiteral("AccountEmail"));
    m_email->setText(m_settings.accountEmail());
    m_password = new QLineEdit(form);
    m_password->setObjectName(QStringLiteral("AccountPassword"));
    m_password->setEchoMode(QLineEdit::Password);
    m_deviceName = new QLineEdit(form);
    m_deviceName->setObjectName(QStringLiteral("AccountDeviceName"));
    // Pre-fill the name the user chose last time rather than the hostname: the
    // device name is this install's identity on the account, and silently
    // snapping it back to the machine's hostname on every sign-in would change
    // it out from under the user (and rename the device on the account).
    m_deviceName->setText(m_settings.accountDeviceName().isEmpty()
                              ? QHostInfo::localHostName()
                              : m_settings.accountDeviceName());

    fields->addRow(tr("Server:"), serverChoices);
    fields->addRow(tr("Email:"), m_email);
    fields->addRow(tr("Password:"), m_password);
    fields->addRow(tr("This device:"), m_deviceName);

    m_signIn = new QPushButton(tr("Sign In"), form);
    m_signIn->setDefault(true);
    m_registerButton = new QPushButton(tr("Create Account"), form);
    auto *formButtons = new QHBoxLayout;
    formButtons->addStretch();
    formButtons->addWidget(m_registerButton);
    formButtons->addWidget(m_signIn);
    fields->addRow(formButtons);
    m_pages->addWidget(form);

    // Page 1: signed in.
    auto *account = new QWidget(m_pages);
    auto *accountLayout = new QVBoxLayout(account);
    m_accountLabel = new QLabel(account);
    m_devices = new QListWidget(account);
    m_devices->setToolTip(tr("Double-click a device to browse its shared folders."));

    // Serving half: off by default, so an account with sharing untouched never
    // opens a listening port at all.
    m_shareEnabled = new QCheckBox(tr("Share these folders with my other devices"), account);
    m_shareEnabled->setChecked(m_settings.deviceSharingEnabled());
    m_sharedFolders = new QListWidget(account);
    m_sharedFolders->addItems(m_settings.sharedFolders());
    auto *addFolder = new QPushButton(tr("Add Folder…"), account);
    auto *removeFolder = new QPushButton(tr("Remove"), account);
    auto *shareButtons = new QHBoxLayout;
    shareButtons->addWidget(addFolder);
    shareButtons->addWidget(removeFolder);
    shareButtons->addStretch();

    auto *signOut = new QPushButton(tr("Sign Out"), account);
    auto *removeDevice = new QPushButton(tr("Remove Device"), account);
    auto *accountButtons = new QHBoxLayout;
    accountButtons->addWidget(removeDevice);
    accountButtons->addStretch();
    accountButtons->addWidget(signOut);
    accountLayout->addWidget(m_accountLabel);
    accountLayout->addWidget(new QLabel(tr("Devices on this account:"), account));
    accountLayout->addWidget(m_devices, 1);
    accountLayout->addWidget(m_shareEnabled);
    accountLayout->addWidget(m_sharedFolders, 1);
    accountLayout->addLayout(shareButtons);
    accountLayout->addLayout(accountButtons);
    m_pages->addWidget(account);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("AccountStatus"));
    m_status->setWordWrap(true);
    auto *closeBox = new QDialogButtonBox(QDialogButtonBox::Close, this);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_pages, 1);
    layout->addWidget(m_status);
    layout->addWidget(closeBox);

    connect(closeBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_customServer, &QRadioButton::toggled, this,
            [this] { updateServerControls(); });
    connect(m_shareEnabled, &QCheckBox::toggled, this,
            [this](bool on) { m_settings.setDeviceSharingEnabled(on); });
    connect(addFolder, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Share Folder"));
        if (dir.isEmpty())
            return;
        if (!m_sharedFolders->findItems(dir, Qt::MatchExactly).isEmpty())
            return;
        m_sharedFolders->addItem(dir);
        saveSharedFolders();
    });
    connect(removeFolder, &QPushButton::clicked, this, [this] {
        delete m_sharedFolders->takeItem(m_sharedFolders->currentRow());
        saveSharedFolders();
    });
    connect(removeDevice, &QPushButton::clicked, this, [this] {
        QListWidgetItem *item = m_devices->currentItem();
        // No id means this machine or the placeholder row. Signing this device
        // out is what the Sign Out button is for, and doing it from here would
        // leave the keyring token behind.
        const QString id = item ? item->data(Qt::UserRole).toString() : QString();
        if (id.isEmpty())
            return;
        const QString name = item->data(Qt::UserRole + 2).toString();
        if (QMessageBox::question(this, tr("FileCommander Account"),
                                  tr("Sign %1 out of this account?").arg(name))
            != QMessageBox::Yes)
            return;
        m_client.removeDevice(id);
    });
    connect(m_devices, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        const QString id = item->data(Qt::UserRole).toString();
        if (id.isEmpty()) // this device, or the placeholder row
            return;
        if (!item->data(Qt::UserRole + 1).toBool()) {
            QMessageBox::information(this, tr("FileCommander Account"),
                                     tr("%1 is offline.").arg(item->data(Qt::UserRole + 2).toString()));
            return;
        }
        emit openDevice(id, item->data(Qt::UserRole + 2).toString());
        accept();
    });
    connect(m_signIn, &QPushButton::clicked, this, [this] {
        if (!applyServerSelection())
            return;
        setBusy(true);
        m_status->setText(tr("Signing in…"));
        m_client.login(m_email->text().trimmed(), m_password->text(),
                       m_deviceName->text().trimmed(), m_settings.accountDeviceId());
    });
    connect(m_registerButton, &QPushButton::clicked, this, [this] {
        if (!applyServerSelection())
            return;
        setBusy(true);
        m_status->setText(tr("Creating account…"));
        m_client.registerAccount(m_email->text().trimmed(), m_password->text());
    });
    connect(signOut, &QPushButton::clicked, this, [this] { m_client.logout(); });

    connect(&m_client, &AccountClient::registered, this, [this](const QString &email) {
        // Registration does not sign in: go straight on to it, so creating an
        // account is one click rather than two.
        setBusy(true);
        m_status->setText(tr("Account created, signing in…"));
        m_settings.setAccountEmail(email);
        m_client.login(email, m_password->text(), m_deviceName->text().trimmed(),
                       m_settings.accountDeviceId());
    });
    connect(&m_client, &AccountClient::loggedIn, this, [this](const AccountInfo &info) {
        m_password->clear();
        m_settings.setAccountEmail(info.email);
        m_settings.setAccountDeviceId(info.deviceId);
        // Remember what this install signed in as, so the next sign-in (and any
        // re-registration after a sign-out) offers that name, not the hostname.
        m_settings.setAccountDeviceName(m_deviceName->text().trimmed());
        setBusy(false);
        m_status->clear();
        showCurrentState();
    });
    connect(&m_client, &AccountClient::loggedOut, this, [this] {
        // The device id stays: it is this machine's identity on the account, not
        // a credential, and reusing it keeps a later sign-in from adding a
        // second row for the same machine.
        setBusy(false);
        m_status->clear();
        showCurrentState();
    });
    connect(&m_client, &AccountClient::requestFailed, this, &AccountDialog::reportError);
    // The protocol version gate is not a sign-in failure: a modal prompt with a
    // title and icon, not a line of status text, because the only fix is to
    // update and there is nothing the user can type to get past it.
    connect(&m_client, &AccountClient::updateRequired, this, [this](const QString &detail) {
        setBusy(false);
        m_status->clear();
        QMessageBox::warning(this, tr("FileCommander Account"),
                             detail.isEmpty()
                                 ? tr("This version of FileCommander can no longer be used. "
                                      "Please update to the latest version.")
                                 : detail);
    });
    connect(&m_client, &AccountClient::devicesReady, this,
            [this](const QVector<AccountDeviceInfo> &devices) {
                m_devices->clear();
                for (const AccountDeviceInfo &d : devices) {
                    const QString name = d.name.isEmpty() ? d.id : d.name;
                    QString label = name;
                    if (d.self)
                        label += tr(" (this device)");
                    label += d.online ? tr(" — online") : tr(" — offline");
                    auto *item = new QListWidgetItem(label, m_devices);
                    // The id is what a session is opened against, and this
                    // machine gets none: browsing yourself over the network
                    // would be a loop, not a feature.
                    if (!d.self)
                        item->setData(Qt::UserRole, d.id);
                    item->setData(Qt::UserRole + 1, d.online);
                    item->setData(Qt::UserRole + 2, name);
                    // Show what the peer is serving before the user commits to
                    // opening a tab, so the device row itself is the usage entry.
                    if (!d.shares.isEmpty())
                        item->setToolTip(tr("Shares: %1").arg(d.shares.join(QLatin1String(", "))));
                }
            });

    updateServerControls();
    showCurrentState();
}

void AccountDialog::showCurrentState() {
    const bool in = m_client.isLoggedIn();
    m_pages->setCurrentIndex(in ? 1 : 0);
    if (!in)
        return;
    m_accountLabel->setText(tr("Signed in as %1").arg(m_client.account().email));
    m_devices->clear();
    m_devices->addItem(tr("Loading…"));
    m_client.fetchDevices();
}

void AccountDialog::updateServerControls() {
    const bool custom = m_customServer->isChecked();
    m_customServerUrl->setVisible(custom);
    m_customServerUrl->setEnabled(custom && m_signIn->isEnabled());
}

bool AccountDialog::applyServerSelection() {
    const bool official = m_officialServer->isChecked();
    const QString customUrl = normalizedServerUrl(m_customServerUrl->text());
    if (!official && !isValidCustomServerUrl(customUrl)) {
        m_status->setText(tr("Enter a valid server URL."));
        m_customServerUrl->setFocus();
        return false;
    }

    const bool wasOfficial = m_settings.accountUsesOfficialServer();
    const QString oldCustom = m_settings.accountCustomServerUrl();
    const bool endpointChanged =
        wasOfficial != official || (!official && oldCustom != customUrl);

    // Refresh tokens are keyed by device id rather than host. Never carry one
    // across a deliberate endpoint change.
    if (endpointChanged && !m_settings.accountDeviceId().isEmpty())
        AccountClient::forgetStoredSession(m_settings.accountDeviceId());

    m_settings.setAccountUsesOfficialServer(official);
    if (!official)
        m_settings.setAccountCustomServerUrl(customUrl);
    const QString nextUrl = official ? QString() : customUrl;
    if (endpointChanged)
        m_client.switchApiUrl(nextUrl);
    else
        m_client.setApiUrl(nextUrl);
    return true;
}

void AccountDialog::saveSharedFolders() {
    QStringList folders;
    for (int i = 0; i < m_sharedFolders->count(); ++i)
        folders.append(m_sharedFolders->item(i)->text());
    m_settings.setSharedFolders(folders);
}

void AccountDialog::setBusy(bool busy) {
    m_signIn->setEnabled(!busy);
    m_registerButton->setEnabled(!busy);
    m_officialServer->setEnabled(!busy);
    m_customServer->setEnabled(!busy);
    m_email->setEnabled(!busy);
    m_password->setEnabled(!busy);
    m_deviceName->setEnabled(!busy);
    updateServerControls();
}

void AccountDialog::reportError(const QString &error) {
    setBusy(false);
    m_status->setText(error);
}
