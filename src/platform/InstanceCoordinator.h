#pragma once

#include <QObject>
#include <QStringList>

#include <memory>

class QLockFile;
class QLocalServer;
class QLocalSocket;

// Routes later shell activations to the first FileCommander process for this
// user, preventing a folder association from creating duplicate main windows.
class InstanceCoordinator final : public QObject {
    Q_OBJECT

public:
    enum class StartResult { Primary, Forwarded, Failed };

    explicit InstanceCoordinator(QString serverName = {}, QObject *parent = nullptr);
    ~InstanceCoordinator() override;

    StartResult startOrActivate(const QStringList &arguments);
    bool isPrimary() const;

signals:
    void activationRequested(const QStringList &arguments);

private:
    static QString defaultServerName();
    QString lockFilePath() const;
    bool forwardToPrimary(const QStringList &arguments) const;
    void handleNewConnection();
    void readActivation(QLocalSocket *socket);

    QString m_serverName;
    std::unique_ptr<QLockFile> m_lock;
    QLocalServer *m_server = nullptr;
};
