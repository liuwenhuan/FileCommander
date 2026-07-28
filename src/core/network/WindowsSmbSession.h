#pragma once

#include <QMutex>
#include <QSet>
#include <QString>

class WindowsSmbSession {
public:
    WindowsSmbSession() = default;
    ~WindowsSmbSession();

    void setCredentials(const QString &user, const QString &password, bool anonymous);
    bool ensureConnected(const QString &uncShare, QString *error);
    void disconnectOwned();

private:
    Q_DISABLE_COPY(WindowsSmbSession)

    QMutex m_mutex;
    QString m_user;
    QString m_password;
    bool m_anonymous = false;
    QSet<QString> m_ownedConnections;
};
