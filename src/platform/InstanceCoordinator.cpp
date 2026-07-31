#include "InstanceCoordinator.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QLockFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QStandardPaths>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
constexpr int kConnectTimeoutMs = 400;
constexpr int kWriteTimeoutMs = 1500;

QByteArray activationPayload(const QStringList &arguments) {
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_5_15);
    stream << arguments;
    return payload;
}

void allowPrimaryToSetForeground(QLockFile *lock) {
#ifdef Q_OS_WIN
    if (!lock)
        return;
    qint64 pid = 0;
    QString hostname;
    QString appname;
    if (!lock->getLockInfo(&pid, &hostname, &appname) || pid <= 0)
        return;
    AllowSetForegroundWindow(static_cast<DWORD>(pid));
#else
    Q_UNUSED(lock);
#endif
}
} // namespace

InstanceCoordinator::InstanceCoordinator(QString serverName, QObject *parent)
    : QObject(parent), m_serverName(serverName.isEmpty() ? defaultServerName() : std::move(serverName)) {}

InstanceCoordinator::~InstanceCoordinator() {
    if (m_server) {
        m_server->close();
        QLocalServer::removeServer(m_serverName);
    }
}

InstanceCoordinator::StartResult InstanceCoordinator::startOrActivate(const QStringList &arguments) {
    m_lock = std::make_unique<QLockFile>(lockFilePath());
    if (!m_lock->tryLock(0)) {
        // The primary may still be setting up its event loop. It already owns
        // the process role, so never turn this late activation into a second
        // window merely because its message cannot be delivered immediately.
        allowPrimaryToSetForeground(m_lock.get());
        forwardToPrimary(arguments);
        m_lock.reset();
        return StartResult::Forwarded;
    }

    m_server = new QLocalServer(this);
    if (!m_server->listen(m_serverName)) {
        // We own the lock, so any leftover socket belongs to a previous crash.
        QLocalServer::removeServer(m_serverName);
        if (!m_server->listen(m_serverName)) {
            delete m_server;
            m_server = nullptr;
            m_lock.reset();
            return StartResult::Failed;
        }
    }
    connect(m_server, &QLocalServer::newConnection, this, &InstanceCoordinator::handleNewConnection);
    return StartResult::Primary;
}

bool InstanceCoordinator::isPrimary() const {
    return m_server && m_server->isListening();
}

QString InstanceCoordinator::defaultServerName() {
    const QByteArray scope = QStandardPaths::writableLocation(
                                 QStandardPaths::AppDataLocation)
                                 .toUtf8();
    const QByteArray userHash = QCryptographicHash::hash(scope, QCryptographicHash::Sha256).toHex();
    return QStringLiteral("FileCommander-") + QString::fromLatin1(userHash.left(24));
}

QString InstanceCoordinator::lockFilePath() const {
    const QByteArray nameHash = QCryptographicHash::hash(m_serverName.toUtf8(),
                                                          QCryptographicHash::Sha256)
                                    .toHex();
    QString directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (directory.isEmpty())
        directory = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    QDir().mkpath(directory);
    return QDir(directory).filePath(QStringLiteral("instance-") +
                                    QString::fromLatin1(nameHash.left(24)) +
                                    QStringLiteral(".lock"));
}

bool InstanceCoordinator::forwardToPrimary(const QStringList &arguments) const {
    QLocalSocket socket;
    socket.connectToServer(m_serverName, QIODevice::ReadWrite);
    if (!socket.waitForConnected(kConnectTimeoutMs))
        return false;

    const QByteArray payload = activationPayload(arguments);
    if (socket.write(payload) != payload.size() || !socket.waitForBytesWritten(kWriteTimeoutMs))
        return false;
    socket.disconnectFromServer();
    return true;
}

void InstanceCoordinator::handleNewConnection() {
    while (m_server && m_server->hasPendingConnections()) {
        QLocalSocket *socket = m_server->nextPendingConnection();
        connect(socket, &QLocalSocket::readyRead, this,
                [this, socket] { readActivation(socket); });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
            readActivation(socket);
            socket->deleteLater();
        });
        readActivation(socket);
    }
}

void InstanceCoordinator::readActivation(QLocalSocket *socket) {
    if (!socket || socket->property("activationHandled").toBool())
        return;

    QByteArray payload = socket->property("activationPayload").toByteArray();
    payload.append(socket->readAll());
    socket->setProperty("activationPayload", payload);
    QDataStream stream(payload);
    stream.setVersion(QDataStream::Qt_5_15);
    QStringList arguments;
    stream >> arguments;
    if (stream.status() != QDataStream::Ok)
        return;

    socket->setProperty("activationHandled", true);
    emit activationRequested(arguments);
}
