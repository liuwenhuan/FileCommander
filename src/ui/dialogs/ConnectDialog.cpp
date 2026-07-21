#include "ConnectDialog.h"

#include "network/CurlFtpProvider.h"
#include "network/CurlWebDavProvider.h"
#include "network/GvfsMounter.h"
#include "network/SftpProvider.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
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
    {"SMB / Windows share", GvfsMounter::Protocol::Smb},
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

} // namespace

ConnectDialog::ConnectDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("Connect to Server"));
    setModal(true);

    m_protocolCombo = new QComboBox(this);
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
    m_buttons->button(QDialogButtonBox::Ok)->setText(tr("Connect"));

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
    auto *hint = new QLabel(
        tr("The server is mounted via GVfs and opened as a local folder."), this);
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
        QMessageBox::warning(this, tr("Save Connection"),
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
    const QString name = QInputDialog::getText(
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
    if (QMessageBox::question(this, tr("Delete Connection"),
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
    m_portSpin->setValue(GvfsMounter::defaultPort(kProtocols[index].protocol));
}

void ConnectDialog::onAnonymousToggled(bool anonymous) {
    m_userEdit->setEnabled(!anonymous);
    m_passwordEdit->setEnabled(!anonymous);
    if (anonymous) {
        m_userEdit->clear();
        m_passwordEdit->clear();
    }
}

void ConnectDialog::accept() {
    const int index = m_protocolCombo->currentIndex();
    if (index < 0 || index >= static_cast<int>(std::size(kProtocols)))
        return;
    const GvfsMounter::Protocol protocol = kProtocols[index].protocol;

    const QString host = m_hostEdit->text().trimmed();
    if (host.isEmpty()) {
        QMessageBox::warning(this, tr("Connect to Server"),
                             tr("Please enter a server address."));
        return;
    }

    const bool anonymous = m_anonymousCheck->isChecked();
    const QString user = anonymous ? QString() : m_userEdit->text().trimmed();
    const QString password = anonymous ? QString() : m_passwordEdit->text();

    // SFTP uses the native libssh2 backend (SftpProvider) rather than a gvfs
    // mount: connect and, on success, hand the connected provider + initial
    // path back to the caller, which swaps it into the panel's model.
    if (protocol == GvfsMounter::Protocol::Sftp) {
        auto provider = std::make_shared<SftpProvider>();
        QString error;
        setEnabled(false);
        QApplication::setOverrideCursor(Qt::WaitCursor);
        const bool ok =
            provider->connectToHost(host, m_portSpin->value(), user, password, &error);
        QApplication::restoreOverrideCursor();
        setEnabled(true);
        if (!ok) {
            QMessageBox::critical(this, tr("Connection Failed"),
                                  tr("Could not connect to %1.\n\n%2").arg(host, error));
            return;
        }
        m_remoteProvider = provider;
        const QString p = m_pathEdit->text().trimmed();
        m_remotePath = p.isEmpty() ? QStringLiteral("/") : p;
        QDialog::accept();
        return;
    }

    // FTP uses the native libcurl backend (CurlFtpProvider) rather than a gvfs
    // mount, mirroring the SFTP branch above: connect and, on success, hand
    // the connected provider + initial path back to the caller.
    if (protocol == GvfsMounter::Protocol::Ftp) {
        auto provider = std::make_shared<CurlFtpProvider>();
        QString error;
        setEnabled(false);
        QApplication::setOverrideCursor(Qt::WaitCursor);
        const bool ok =
            provider->connectToHost(host, m_portSpin->value(), user, password, &error);
        QApplication::restoreOverrideCursor();
        setEnabled(true);
        if (!ok) {
            QMessageBox::critical(this, tr("Connection Failed"),
                                  tr("Could not connect to %1.\n\n%2").arg(host, error));
            return;
        }
        m_remoteProvider = provider;
        const QString p = m_pathEdit->text().trimmed();
        m_remotePath = p.isEmpty() ? QStringLiteral("/") : p;
        QDialog::accept();
        return;
    }

    // WebDAV (HTTP/HTTPS) likewise uses the native libcurl backend
    // (CurlWebDavProvider), same mechanism as SFTP/FTP above.
    if (protocol == GvfsMounter::Protocol::WebDav ||
        protocol == GvfsMounter::Protocol::WebDavs) {
        const bool useHttps = protocol == GvfsMounter::Protocol::WebDavs;
        auto provider = std::make_shared<CurlWebDavProvider>();
        QString error;
        setEnabled(false);
        QApplication::setOverrideCursor(Qt::WaitCursor);
        const bool ok = provider->connectToHost(host, m_portSpin->value(), user, password,
                                                useHttps, &error);
        QApplication::restoreOverrideCursor();
        setEnabled(true);
        if (!ok) {
            QMessageBox::critical(this, tr("Connection Failed"),
                                  tr("Could not connect to %1.\n\n%2").arg(host, error));
            return;
        }
        m_remoteProvider = provider;
        const QString p = m_pathEdit->text().trimmed();
        m_remotePath = p.isEmpty() ? QStringLiteral("/") : p;
        QDialog::accept();
        return;
    }

    const QString uri = GvfsMounter::buildUri(protocol, host, m_portSpin->value(),
                                              user, m_pathEdit->text());
    if (uri.isEmpty()) {
        QMessageBox::warning(this, tr("Connect to Server"),
                             tr("Could not build a connection URI."));
        return;
    }

    // Mounting can block briefly; give the user a busy cursor.
    setEnabled(false);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const GvfsMounter::MountResult result = GvfsMounter::mount(uri, password);
    QApplication::restoreOverrideCursor();
    setEnabled(true);

    if (!result.success) {
        QMessageBox::critical(
            this, tr("Connection Failed"),
            tr("Could not connect to %1.\n\n%2").arg(uri, result.error));
        return;
    }

    m_mountedUri = result.uri;
    m_mountedLocalPath = result.localPath;
    QDialog::accept();
}

QString ConnectDialog::runAndMount(QWidget *parent) {
    ConnectDialog dialog(parent);
    if (dialog.exec() == QDialog::Accepted)
        return dialog.mountedLocalPath();
    return QString();
}
