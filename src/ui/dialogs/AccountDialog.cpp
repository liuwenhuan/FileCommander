#include "AccountDialog.h"

#include <QCheckBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHostInfo>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include "account/AccountClient.h"
#include "account/EmailAddress.h"
#include "config/Settings.h"

namespace {

constexpr int kDialogWidth = 500;
constexpr int kSignedOutHeight = 350;
constexpr int kSignedInHeight = 430;

class CurrentPageStack final : public QStackedWidget {
public:
    explicit CurrentPageStack(QWidget *parent) : QStackedWidget(parent) {
        connect(this, &QStackedWidget::currentChanged, this, [this] { updateGeometry(); });
    }

    QSize sizeHint() const override {
        return currentWidget() ? currentWidget()->sizeHint() : QStackedWidget::sizeHint();
    }

    QSize minimumSizeHint() const override {
        return currentWidget() ? currentWidget()->minimumSizeHint()
                               : QStackedWidget::minimumSizeHint();
    }
};

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
    resize(kDialogWidth, kSignedOutHeight);

    m_pages = new CurrentPageStack(this);
    m_pages->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);

    // Page 0: signed out.
    auto *scroll = new QScrollArea(m_pages);
    m_loginScroll = scroll;
    scroll->setObjectName(QStringLiteral("AccountLoginScroll"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *form = new QWidget(scroll);
    auto *formLayout = new QVBoxLayout(form);
    formLayout->setContentsMargins(16, 0, 16, 0);
    formLayout->setSpacing(0);

    auto *serverChoices = new QVBoxLayout;
    serverChoices->setSpacing(0);
    auto *customServerRow = new QHBoxLayout;
    customServerRow->setSpacing(4);
    m_officialServer = new QRadioButton(tr("Official server"), form);
    m_officialServer->setObjectName(QStringLiteral("OfficialServerRadio"));
    m_officialServer->setToolTip(tr("Official server"));
    m_customServer = new QRadioButton(tr("Custom server"), form);
    m_customServer->setObjectName(QStringLiteral("CustomServerRadio"));
    m_customServer->setToolTip(tr("Custom server"));
    m_customServer->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_customServerUrl = new QLineEdit(form);
    m_customServerUrl->setObjectName(QStringLiteral("CustomServerUrl"));
    m_customServerUrl->setPlaceholderText(tr("Server URL"));
    m_customServerUrl->setText(m_settings.accountCustomServerUrl());
    serverChoices->addWidget(m_officialServer);
    customServerRow->addWidget(m_customServer);
    customServerRow->addWidget(m_customServerUrl, 1);
    serverChoices->addLayout(customServerRow);
    const bool official = m_settings.accountUsesOfficialServer();
    m_officialServer->setChecked(official);
    m_customServer->setChecked(!official);

    m_email = new QLineEdit(form);
    m_email->setObjectName(QStringLiteral("AccountEmail"));
    m_email->setAccessibleName(tr("Username"));
    m_email->setPlaceholderText(tr("Email:"));
    m_email->setMinimumWidth(0);
    m_email->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_email->setText(m_settings.accountEmail());
    m_password = new QLineEdit(form);
    m_password->setObjectName(QStringLiteral("AccountPassword"));
    m_password->setAccessibleName(tr("Password:"));
    m_password->setPlaceholderText(tr("Password:"));
    m_password->setMinimumWidth(0);
    m_password->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_password->setEchoMode(QLineEdit::Password);
    m_deviceName = new QLineEdit(form);
    m_deviceName->setObjectName(QStringLiteral("AccountDeviceName"));
    m_deviceName->setAccessibleName(tr("Device name"));
    m_deviceName->setMinimumWidth(0);
    m_deviceName->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    // Pre-fill the name the user chose last time rather than the hostname: the
    // device name is this install's identity on the account, and silently
    // snapping it back to the machine's hostname on every sign-in would change
    // it out from under the user (and rename the device on the account).
    m_deviceName->setText(m_settings.accountDeviceName().isEmpty()
                              ? QHostInfo::localHostName()
                              : m_settings.accountDeviceName());
    m_customServerUrl->setMinimumWidth(100);
    QSizePolicy customUrlPolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    customUrlPolicy.setRetainSizeWhenHidden(true);
    m_customServerUrl->setSizePolicy(customUrlPolicy);

    auto *usernameLabel = new QLabel(tr("Username"), form);
    usernameLabel->setObjectName(QStringLiteral("AccountUsernameLabel"));
    usernameLabel->setBuddy(m_email);
    usernameLabel->setMinimumWidth(0);
    usernameLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    auto *passwordLabel = new QLabel(tr("Password:"), form);
    passwordLabel->setObjectName(QStringLiteral("AccountPasswordLabel"));
    passwordLabel->setBuddy(m_password);
    passwordLabel->setMinimumWidth(0);
    passwordLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    auto *credentialsLabels = new QHBoxLayout;
    credentialsLabels->setSpacing(5);
    credentialsLabels->addWidget(usernameLabel, 1);
    credentialsLabels->addWidget(passwordLabel, 1);
    auto *credentialsRow = new QHBoxLayout;
    credentialsRow->setSpacing(5);
    credentialsRow->addWidget(m_email, 1);
    credentialsRow->addWidget(m_password, 1);

    auto *deviceRow = new QHBoxLayout;
    deviceRow->setSpacing(3);
    auto *deviceLabel = new QLabel(tr("Device name"), form);
    deviceLabel->setObjectName(QStringLiteral("AccountDeviceLabel"));
    deviceRow->addWidget(deviceLabel);
    deviceRow->addWidget(m_deviceName, 1);

    m_signIn = new QPushButton(tr("Sign In"), form);
    m_signIn->setDefault(true);
    m_registerButton = new QPushButton(tr("Create Account"), form);
    auto *actionsRow = new QHBoxLayout;
    actionsRow->setSpacing(5);
    actionsRow->addStretch(1);
    actionsRow->addWidget(m_registerButton);
    actionsRow->addWidget(m_signIn);
    m_status = new QLabel(form);
    m_status->setObjectName(QStringLiteral("AccountStatus"));
    m_status->setWordWrap(true);
    formLayout->addLayout(serverChoices);
    formLayout->addLayout(credentialsLabels);
    formLayout->addLayout(credentialsRow);
    formLayout->addLayout(deviceRow);
    formLayout->addLayout(actionsRow);
    formLayout->addWidget(m_status);
    m_status->hide();
    scroll->setWidget(form);
    m_pages->addWidget(scroll);

    // Page 1: signed in.
    auto *account = new QWidget(m_pages);
    auto *accountLayout = new QVBoxLayout(account);
    accountLayout->setContentsMargins(9, 6, 9, 6);
    accountLayout->setSpacing(4);
    m_accountLabel = new QLabel(account);
    m_accountLabel->setWordWrap(true);
    m_devices = new QListWidget(account);
    m_devices->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    m_devices->setToolTip(tr("Double-click a device to browse its shared folders."));

    // Serving half: off by default, so an account with sharing untouched never
    // opens a listening port at all.
    m_shareEnabled = new QCheckBox(tr("Share these folders with my other devices"), account);
    m_shareEnabled->setChecked(m_settings.deviceSharingEnabled());
    m_sharedFolders = new QListWidget(account);
    m_sharedFolders->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
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
    m_accountStatus = new QLabel(account);
    m_accountStatus->setObjectName(QStringLiteral("AccountSignedInStatus"));
    m_accountStatus->setWordWrap(true);
    m_accountStatus->hide();
    accountLayout->addWidget(m_accountStatus);
    accountLayout->addLayout(accountButtons);
    m_pages->addWidget(account);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_pages, 1);

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
        const auto email = AccountEmail::canonicalize(m_email->text());
        if (!email) {
            setSignedOutStatus(tr("Enter a valid email address."));
            m_email->setFocus();
            return;
        }
        m_email->setText(*email);
        if (!applyServerSelection())
            return;
        setBusy(true);
        setSignedOutStatus(tr("Signing in…"));
        m_client.login(*email, m_password->text(), m_deviceName->text().trimmed(),
                       m_settings.accountDeviceId());
    });
    connect(m_registerButton, &QPushButton::clicked, this, [this] {
        const auto email = AccountEmail::canonicalize(m_email->text());
        if (!email) {
            setSignedOutStatus(tr("Enter a valid email address."));
            m_email->setFocus();
            return;
        }
        m_email->setText(*email);
        if (!applyServerSelection())
            return;
        setBusy(true);
        setSignedOutStatus(tr("Creating account…"));
        m_client.registerAccount(*email, m_password->text());
    });
    connect(signOut, &QPushButton::clicked, this, [this] { m_client.logout(); });

    connect(&m_client, &AccountClient::registered, this, [this](const QString &email) {
        // Registration does not sign in: go straight on to it, so creating an
        // account is one click rather than two.
        setBusy(true);
        setSignedOutStatus(tr("Account created, signing in…"));
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
        setSignedOutStatus(QString());
        m_accountStatus->clear();
        m_accountStatus->hide();
        showCurrentState();
    });
    connect(&m_client, &AccountClient::loggedOut, this, [this] {
        // The device id stays: it is this machine's identity on the account, not
        // a credential, and reusing it keeps a later sign-in from adding a
        // second row for the same machine.
        setBusy(false);
        setSignedOutStatus(QString());
        m_accountStatus->clear();
        m_accountStatus->hide();
        showCurrentState();
    });
    connect(&m_client, &AccountClient::requestFailed, this, &AccountDialog::reportError);
    // The protocol version gate is not a sign-in failure: a modal prompt with a
    // title and icon, not a line of status text, because the only fix is to
    // update and there is nothing the user can type to get past it.
    connect(&m_client, &AccountClient::updateRequired, this, [this](const QString &detail) {
        setBusy(false);
        setSignedOutStatus(QString());
        m_accountStatus->clear();
        m_accountStatus->hide();
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
    layout()->invalidate();
    layout()->activate();
    updateWindowSize();
    if (!in)
        return;
    m_accountLabel->setText(tr("Signed in as %1").arg(m_client.account().email));
    m_devices->clear();
    m_devices->addItem(tr("Loading…"));
    m_client.fetchDevices();
}

QSize AccountDialog::sizeHint() const {
    if (!m_pages)
        return FramelessDialog::sizeHint();
    if (m_pages->currentIndex() == 1)
        return QSize(kDialogWidth, kSignedInHeight);
    return QSize(kDialogWidth, kSignedOutHeight);
}

void AccountDialog::updateWindowSize() {
    const QPoint center = geometry().center();
    const QSize targetSize = sizeHint();
    const QSize oldSize = size();
    if (m_pages->currentIndex() == 0) {
        setFixedSize(targetSize);
    } else {
        setMinimumSize(0, 0);
        setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    }
    if (size() != targetSize)
        resize(targetSize);
    if (isVisible() && size() != oldSize)
        move(center - QPoint((width() - 1) / 2, (height() - 1) / 2));
}

void AccountDialog::updateServerControls() {
    const bool custom = m_customServer->isChecked();
    m_customServerUrl->setVisible(custom);
    m_customServerUrl->setEnabled(custom && m_signIn->isEnabled());
    updateWindowSize();
}

bool AccountDialog::applyServerSelection() {
    const bool official = m_officialServer->isChecked();
    const QString customUrl = normalizedServerUrl(m_customServerUrl->text());
    if (!official && !isValidCustomServerUrl(customUrl)) {
        setSignedOutStatus(tr("Enter a valid server URL."));
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

void AccountDialog::setSignedOutStatus(const QString &message) {
    m_status->setText(message);
    m_status->setVisible(!message.isEmpty());
    updateWindowSize();
    QTimer::singleShot(0, this, [this] {
        if (!m_status->isHidden() && m_pages->currentIndex() == 0)
            m_loginScroll->ensureWidgetVisible(m_status);
    });
}

void AccountDialog::reportError(const QString &error) {
    setBusy(false);
    if (m_pages->currentIndex() == 0) {
        setSignedOutStatus(error);
    } else {
        m_accountStatus->setText(error);
        m_accountStatus->show();
    }
}
