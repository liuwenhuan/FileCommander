#include "ConnectDialog.h"

#include "network/GvfsMounter.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
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

    auto *layout = new QVBoxLayout(this);
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

    onProtocolChanged(0); // seed default port
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
