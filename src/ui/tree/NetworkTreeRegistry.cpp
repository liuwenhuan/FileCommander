#include "NetworkTreeRegistry.h"

#include "network/NetworkSession.h"

NetworkTreeRegistry::NetworkTreeRegistry(QObject *parent) : QObject(parent) {}

void NetworkTreeRegistry::registerConnection(const RegisteredConnection &conn) {
    if (conn.connectionId.isEmpty() || conn.session.expired())
        return;

    bool dirty = pruneExpired();
    for (RegisteredConnection &existing : m_connections) {
        if (existing.connectionId != conn.connectionId)
            continue;
        // Same connection seen again: refresh the presentation fields (the label
        // firms up once connected) without adding a second root for it.
        if (existing.label != conn.label || existing.scheme != conn.scheme
            || existing.basePath != conn.basePath || existing.owner != conn.owner) {
            existing.label = conn.label;
            existing.scheme = conn.scheme;
            existing.basePath = conn.basePath;
            existing.owner = conn.owner;
            dirty = true;
        }
        existing.session = conn.session;
        if (dirty)
            emit changed();
        return;
    }

    m_connections.append(conn);
    emit changed();
}

void NetworkTreeRegistry::unregisterConnection(const QString &connectionId) {
    if (connectionId.isEmpty())
        return;
    bool dirty = pruneExpired();
    for (int i = 0; i < m_connections.size(); ++i) {
        if (m_connections.at(i).connectionId == connectionId) {
            m_connections.remove(i);
            dirty = true;
            break;
        }
    }
    if (dirty)
        emit changed();
}

QVector<RegisteredConnection> NetworkTreeRegistry::connections() {
    if (pruneExpired())
        emit changed();
    return m_connections;
}

std::shared_ptr<NetworkSession> NetworkTreeRegistry::sessionFor(const QString &connectionId) {
    for (const RegisteredConnection &conn : m_connections)
        if (conn.connectionId == connectionId)
            return conn.session.lock();
    return {};
}

bool NetworkTreeRegistry::pruneExpired() {
    const int before = m_connections.size();
    for (int i = m_connections.size() - 1; i >= 0; --i)
        if (m_connections.at(i).session.expired())
            m_connections.remove(i);
    return m_connections.size() != before;
}
