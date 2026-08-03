#include "ConnectDialog.h"

#include "network/CurlFtpProvider.h"
#include "network/CurlWebDavProvider.h"
#include "network/GvfsMounter.h"
#include "network/SftpProvider.h"
#if FILECOMMANDER_HAS_LINUX_INTEGRATION
#include "network/SmbProvider.h"
using NativeSmbProvider = SmbProvider;
#elif defined(Q_OS_WIN)
#include "network/WindowsSmbProvider.h"
using NativeSmbProvider = WindowsSmbProvider;
#endif

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>

#include "ThemedDialogs.h"
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <iterator> // std::size

namespace {

// Combo entries in display order, paired with their protocol + default port.
struct ProtocolChoice {
    const char *label;
    GvfsMounter::Protocol protocol;
};

const ProtocolChoice kProtocols[] = {
    {"SFTP (SSH)", GvfsMounter::Protocol::Sftp},
#if FILECOMMANDER_HAS_LINUX_INTEGRATION || defined(Q_OS_WIN)
    {"SMB / Windows share", GvfsMounter::Protocol::Smb},
#endif
    {"WebDAV (HTTP)", GvfsMounter::Protocol::WebDav},
    {"WebDAV (HTTPS)", GvfsMounter::Protocol::WebDavs},
    {"FTP", GvfsMounter::Protocol::Ftp},
};

// Maps a stored GvfsMounter::Protocol value to its combo index (0 if unknown).
int protocolToIndex(int protocol) {
    for (int i = 0; i < static_cast<int>(std::size(kProtocols)); ++i)
        if (static_cast<int>(kProtocols[i].protocol) == protocol)
            return i;
    return 0;
}

int defaultPort(GvfsMounter::Protocol protocol) {
    switch (protocol) {
    case GvfsMounter::Protocol::Sftp: return 22;
    case GvfsMounter::Protocol::Smb: return 445;
    case GvfsMounter::Protocol::WebDav: return 80;
    case GvfsMounter::Protocol::WebDavs: return 443;
    case GvfsMounter::Protocol::Ftp: return 21;
    }
    return 0;
}

} // namespace

ConnectDialog::ConnectDialog(QWidget *parent) : FramelessDialog(parent) {
    setWindowTitle(tr("Manage Network Connections"));
    setModal(true);

    m_protocolCombo = new QComboBox(this);
    m_protocolCombo->setObjectName(QStringLiteral("protocolCombo"));
    for (const auto &choice : kProtocols)
        m_protocolCombo->addItem(tr(choice.label));

    m_hostEdit = new QLineEdit(this);
    m_hostEdit->setPlaceholderText(tr("example.com or 192.168.1.10"));

    m_portSpin = new QSpinBox(this);
    m_portSpin->setRange(1, 65535);

    m_userEdit = new QLineEdit(this);
    m_passwordEdit = new QLineEdit(this);
    m_passwordEdit->setEchoMode(QLineEdit::Password);

    m_pathEdit = new QLineEdit(this);
    m_pathEdit->setPlaceholderText(tr("/ (remote path or share)"));
    m_pathEdit->setText(QStringLiteral("/"));

    m_anonymousCheck = new QCheckBox(tr("Connect anonymously"), this);

    auto *form = new QFormLayout;
    form->addRow(tr("Protocol:"), m_protocolCombo);
    form->addRow(tr("Server:"), m_hostEdit);
    form->addRow(tr("Port:"), m_portSpin);
    form->addRow(tr("User name:"), m_userEdit);
    form->addRow(tr("Password:"), m_passwordEdit);
    form->addRow(tr("Remote path:"), m_pathEdit);
    form->addRow(QString(), m_anonymousCheck);

    m_buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    ttc::setStandardButtonOverride(m_buttons->button(QDialogButtonBox::Ok), tr("Connect"));
    ttc::localizeStandardButtons(m_buttons);

    // Saved-connections panel: a list of bookmarks with Save / Delete buttons.
    // Selecting one fills the form (and pulls its password from the keyring).
    m_savedList = new QListWidget(this);
    m_savedList->setMaximumHeight(120);
    m_saveButton = new QPushButton(tr("Save"), this);
    m_deleteButton = new QPushButton(tr("Delete"), this);
    m_deleteButton->setEnabled(false);

    auto *savedButtons = new QHBoxLayout;
    savedButtons->addWidget(m_saveButton);
    savedButtons->addWidget(m_deleteButton);
    savedButtons->addStretch();

    auto *savedBox = new QGroupBox(tr("Saved connections"), this);
    auto *savedLayout = new QVBoxLayout(savedBox);
    savedLayout->addWidget(m_savedList);
    savedLayout->addLayout(savedButtons);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(savedBox);
    layout->addLayout(form);
#if FILECOMMANDER_HAS_LINUX_INTEGRATION || defined(Q_OS_WIN)
    const QString hintText =
        tr("SFTP, FTP, WebDAV and SMB all connect through a built-in client.");
#else
    const QString hintText =
        tr("SFTP, FTP and WebDAV connect through built-in cross-platform clients.");
#endif
    auto *hint = new QLabel(hintText, this);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addWidget(m_buttons);

    connect(m_buttons, &QDialogButtonBox::accepted, this, &ConnectDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &ConnectDialog::reject);
    connect(m_protocolCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &ConnectDialog::onProtocolChanged);
    connect(m_anonymousCheck, &QCheckBox::toggled, this,
            &ConnectDialog::onAnonymousToggled);
    connect(m_savedList, &QListWidget::itemSelectionChanged, this,
            &ConnectDialog::onSavedSelectionChanged);
    connect(m_saveButton, &QPushButton::clicked, this, &ConnectDialog::onSaveConnection);
    connect(m_deleteButton, &QPushButton::clicked, this, &ConnectDialog::onDeleteConnection);

    onProtocolChanged(0); // seed default port
    reloadSavedList();
}

void ConnectDialog::changeEvent(QEvent *event) {
    FramelessDialog::changeEvent(event);
    if (event->type() != QEvent::LanguageChange || !m_buttons)
        return;

    ttc::setStandardButtonOverride(m_buttons->button(QDialogButtonBox::Ok), tr("Connect"));
    ttc::localizeStandardButtons(m_buttons);
}

void ConnectDialog::reloadSavedList() {
    m_saved = ConnectionStore::loadAll();
    m_savedList->clear();
    for (const SavedConnection &c : m_saved) {
        const QString label = c.name.isEmpty() ? c.host : c.name;
        m_savedList->addItem(label);
    }
    m_deleteButton->setEnabled(false);
}

void ConnectDialog::fillForm(const SavedConnection &conn) {
    m_protocolCombo->setCurrentIndex(protocolToIndex(conn.protocol));
    m_hostEdit->setText(conn.host);
    if (conn.port > 0)
        m_portSpin->setValue(conn.port);
    m_userEdit->setText(conn.user);
    m_pathEdit->setText(conn.remotePath.isEmpty() ? QStringLiteral("/") : conn.remotePath);
    m_anonymousCheck->setChecked(conn.anonymous);
    // Password lives in the keyring, not the INI.
    m_passwordEdit->setText(ConnectionStore::loadPassword(conn.id));
}

SavedConnection ConnectDialog::currentFormAsConnection() const {
    SavedConnection c;
    const int index = m_protocolCombo->currentIndex();
    if (index >= 0 && index < static_cast<int>(std::size(kProtocols)))
        c.protocol = static_cast<int>(kProtocols[index].protocol);
    c.host = m_hostEdit->text().trimmed();
    c.port = m_portSpin->value();
    c.anonymous = m_anonymousCheck->isChecked();
    c.user = c.anonymous ? QString() : m_userEdit->text().trimmed();
    const QString p = m_pathEdit->text().trimmed();
    c.remotePath = p.isEmpty() ? QStringLiteral("/") : p;
    return c;
}

void ConnectDialog::onSavedSelectionChanged() {
    const int row = m_savedList->currentRow();
    if (row < 0 || row >= m_saved.size()) {
        m_deleteButton->setEnabled(false);
        return;
    }
    m_currentId = m_saved[row].id;
    m_deleteButton->setEnabled(true);
    fillForm(m_saved[row]);
}

void ConnectDialog::onSaveConnection() {
    SavedConnection c = currentFormAsConnection();
    if (c.host.isEmpty()) {
        ttc::warning(this, tr("Save Connection"),
                             tr("Please enter a server address first."));
        return;
    }

    // Reuse the selected bookmark's id (update in place) or start a new one.
    c.id = m_currentId;
    const SavedConnection existing =
        c.id.isEmpty() ? SavedConnection{} : ConnectionStore::load(c.id);
    const QString suggested =
        !existing.name.isEmpty() ? existing.name : c.host;

    bool ok = false;
    const QString name = ttc::getText(
        this, tr("Save Connection"), tr("Name for this connection:"),
        QLineEdit::Normal, suggested, &ok);
    if (!ok)
        return;
    c.name = name.trimmed().isEmpty() ? c.host : name.trimmed();

    const QString id = ConnectionStore::save(c);
    // Store the password (or clear it) in the keyring alongside the metadata.
    const QString password = c.anonymous ? QString() : m_passwordEdit->text();
    if (password.isEmpty())
        ConnectionStore::clearPassword(id);
    else
        ConnectionStore::storePassword(id, password);

    m_currentId = id;
    reloadSavedList();
    // Reselect the just-saved bookmark.
    for (int i = 0; i < m_saved.size(); ++i) {
        if (m_saved[i].id == id) {
            m_savedList->setCurrentRow(i);
            break;
        }
    }
}

void ConnectDialog::onDeleteConnection() {
    const int row = m_savedList->currentRow();
    if (row < 0 || row >= m_saved.size())
        return;
    const SavedConnection &c = m_saved[row];
    const QString label = c.name.isEmpty() ? c.host : c.name;
    if (ttc::question(this, tr("Delete Connection"),
                              tr("Remove the saved connection \"%1\"?").arg(label)) !=
        QMessageBox::Yes)
        return;
    ConnectionStore::remove(c.id);
    if (m_currentId == c.id)
        m_currentId.clear();
    reloadSavedList();
}

void ConnectDialog::onProtocolChanged(int index) {
    if (index < 0 || index >= static_cast<int>(std::size(kProtocols)))
        return;
    m_portSpin->setValue(defaultPort(kProtocols[index].protocol));
}

void ConnectDialog::onAnonymousToggled(bool anonymous) {
    m_userEdit->setEnabled(!anonymous);
    m_passwordEdit->setEnabled(!anonymous);
    if (anonymous) {
        m_userEdit->clear();
        m_passwordEdit->clear();
    }
}

void ConnectDialog::selectProtocol(int protocol) {
    m_protocolCombo->setCurrentIndex(protocolToIndex(protocol));
}

void ConnectDialog::accept() {
    const int index = m_protocolCombo->currentIndex();
    if (index < 0 || index >= static_cast<int>(std::size(kProtocols)))
        return;
    const GvfsMounter::Protocol protocol = kProtocols[index].protocol;

    const QString host = m_hostEdit->text().trimmed();
    if (host.isEmpty()) {
        ttc::warning(this, tr("Connect to Server"),
                             tr("Please enter a server address."));
        return;
    }

    const bool anonymous = m_anonymousCheck->isChecked();
    const QString user = anonymous ? QString() : m_userEdit->text().trimmed();
    const QString password = anonymous ? QString() : m_passwordEdit->text();

    const int port = m_portSpin->value();

    // The tab label to show immediately while connecting -- same "user@host"
    // shape the providers' displayName() returns once connected, so the tab
    // doesn't jump when the link comes up.
    m_displayLabel = user.isEmpty() ? host : user + QLatin1Char('@') + host;

    // Reconnect descriptor for session persistence (so this server -- and its tab
    // label -- return on next launch). Password is NOT stored here; m_currentId
    // (set when a bookmark was loaded) lets the keyring password be looked up.
    m_connInfo = SavedConnection{};
    m_connInfo.protocol = static_cast<int>(protocol);
    m_connInfo.host = host;
    m_connInfo.port = port;
    m_connInfo.user = user;
    m_connInfo.anonymous = anonymous;
    m_connInfo.id = m_currentId;
    {
        const QString rp = m_pathEdit->text().trimmed();
        m_connInfo.remotePath = rp.isEmpty() ? QStringLiteral("/") : rp;
    }

    // Native backends (SFTP/FTP/WebDAV/SMB): DON'T connect here -- that would
    // block the GUI thread on the network. Build the (unconnected) provider and
    // a connect closure; the caller hands both to FileSystemModel::connectNetwork
    // which runs the connect on a worker thread and shows progress in the status
    // line. accept() returns immediately.

    // SFTP (libssh2 / SftpProvider).
    if (protocol == GvfsMounter::Protocol::Sftp) {
        auto provider = std::make_shared<SftpProvider>();
        m_remoteProvider = provider;
        m_connectFn = [provider, host, port, user, password](QString *error) {
            return provider->connectToHost(host, port, user, password, error);
        };
        m_authFactory = [provider, host, port](const QString &u, const QString &pw) {
            return std::function<bool(QString *)>(
                [provider, host, port, u, pw](QString *e) {
                    return provider->connectToHost(host, port, u, pw, e);
                });
        };
        const QString p = m_pathEdit->text().trimmed();
        m_remotePath = p.isEmpty() ? QStringLiteral("/") : p;
        QDialog::accept();
        return;
    }

    // FTP (libcurl / CurlFtpProvider).
    if (protocol == GvfsMounter::Protocol::Ftp) {
        auto provider = std::make_shared<CurlFtpProvider>();
        m_remoteProvider = provider;
        m_connectFn = [provider, host, port, user, password](QString *error) {
            return provider->connectToHost(host, port, user, password, error);
        };
        m_authFactory = [provider, host, port](const QString &u, const QString &pw) {
            return std::function<bool(QString *)>(
                [provider, host, port, u, pw](QString *e) {
                    return provider->connectToHost(host, port, u, pw, e);
                });
        };
        const QString p = m_pathEdit->text().trimmed();
        m_remotePath = p.isEmpty() ? QStringLiteral("/") : p;
        QDialog::accept();
        return;
    }

    // WebDAV / WebDAVS (libcurl / CurlWebDavProvider).
    if (protocol == GvfsMounter::Protocol::WebDav ||
        protocol == GvfsMounter::Protocol::WebDavs) {
        const bool useHttps = protocol == GvfsMounter::Protocol::WebDavs;
        auto provider = std::make_shared<CurlWebDavProvider>();
        m_remoteProvider = provider;
        m_connectFn = [provider, host, port, user, password, useHttps](QString *error) {
            return provider->connectToHost(host, port, user, password, useHttps, error);
        };
        m_authFactory = [provider, host, port, useHttps](const QString &u, const QString &pw) {
            return std::function<bool(QString *)>(
                [provider, host, port, u, pw, useHttps](QString *e) {
                    return provider->connectToHost(host, port, u, pw, useHttps, e);
                });
        };
        const QString p = m_pathEdit->text().trimmed();
        m_remotePath = p.isEmpty() ? QStringLiteral("/") : p;
        QDialog::accept();
        return;
    }

    // SMB/CIFS (libsmbclient / SmbProvider). "/" lists the shares.
#if FILECOMMANDER_HAS_LINUX_INTEGRATION || defined(Q_OS_WIN)
    if (protocol == GvfsMounter::Protocol::Smb) {
        auto provider = std::make_shared<NativeSmbProvider>();
        m_remoteProvider = provider;
        m_connectFn = [provider, host, user, password, anonymous](QString *error) {
            return provider->connectToHost(host, user, password, QString(), anonymous, error);
        };
        m_authFactory = [provider, host](const QString &u, const QString &pw) {
            return std::function<bool(QString *)>(
                [provider, host, u, pw](QString *e) {
                    return provider->connectToHost(host, u, pw, QString(), /*anonymous=*/false, e);
                });
        };
        const QString p = m_pathEdit->text().trimmed();
        m_remotePath = p.isEmpty() ? QStringLiteral("/") : p;
        QDialog::accept();
        return;
    }
#endif

#if FILECOMMANDER_HAS_LINUX_INTEGRATION
    const QString uri = GvfsMounter::buildUri(protocol, host, m_portSpin->value(),
                                              user, m_pathEdit->text());
    if (uri.isEmpty()) {
        ttc::warning(this, tr("Connect to Server"),
                             tr("Could not build a connection URI."));
        return;
    }

    // Mounting can block briefly; give the user a busy cursor.
    setEnabled(false);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    RemoteLocation loc;
    loc.scheme = GvfsMounter::scheme(protocol);
    loc.host = host;
    loc.port = m_portSpin->value();
    loc.user = user;
    loc.password = password;
    loc.anonymous = user.isEmpty();
    const GvfsMounter::MountResult result = GvfsMounter::mount(uri, loc);
    QApplication::restoreOverrideCursor();
    setEnabled(true);

    if (!result.success) {
        ttc::critical(
            this, tr("Connection Failed"),
            tr("Could not connect to %1.\n\n%2").arg(uri, result.error));
        return;
    }

    m_mountedUri = result.uri;
    m_mountedLocalPath = result.localPath;
    QDialog::accept();
#endif
}

QString ConnectDialog::runAndMount(QWidget *parent) {
    ConnectDialog dialog(parent);
    if (dialog.exec() == QDialog::Accepted)
        return dialog.mountedLocalPath();
    return QString();
}
