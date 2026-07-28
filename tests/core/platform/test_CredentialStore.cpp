#include <gtest/gtest.h>

#include "CredentialStore.h"
#include "Settings.h"
#include "network/ConnectionStore.h"

#include <QFile>
#include <QTemporaryDir>
#include <QUuid>

namespace {
class IsolatedConnections {
public:
    IsolatedConnections() {
        qputenv("FILECOMMANDER_CONFIG_HOME", directory.path().toUtf8());
    }
    ~IsolatedConnections() { qunsetenv("FILECOMMANDER_CONFIG_HOME"); }
    QTemporaryDir directory;
};
}

TEST(ConnectionStore, PersistsBookmarkMetadataWithoutPassword) {
    IsolatedConnections isolated;
    SavedConnection connection;
    connection.name = QStringLiteral("测试服务器");
    connection.protocol = 1;
    connection.host = QStringLiteral("nas");
    connection.user = QStringLiteral("alice");
    connection.remotePath = QStringLiteral("/share");

    const QString id = ConnectionStore::save(connection);
    ASSERT_FALSE(id.isEmpty());
    const SavedConnection loaded = ConnectionStore::load(id);
    EXPECT_EQ(loaded.name, connection.name);
    EXPECT_EQ(loaded.host, connection.host);

    QFile settings(Settings::configFilePath());
    ASSERT_TRUE(settings.open(QIODevice::ReadOnly));
    const QByteArray bytes = settings.readAll();
    EXPECT_FALSE(bytes.contains("password"));
    EXPECT_FALSE(bytes.contains("secret-value"));
}

#if defined(Q_OS_WIN)
TEST(CredentialStore, RoundTripsAndRemovesUnicodeSecret) {
    const QString id =
        QStringLiteral("test-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString secret = QStringLiteral("密码-🔐-secret-value");

    ASSERT_TRUE(CredentialStore::save(id, secret).ok);
    QString loaded;
    ASSERT_TRUE(CredentialStore::load(id, &loaded).ok);
    EXPECT_EQ(loaded, secret);
    ASSERT_TRUE(CredentialStore::remove(id).ok);
    loaded = QStringLiteral("unchanged");
    const PlatformResult missing = CredentialStore::load(id, &loaded);
    EXPECT_FALSE(missing.ok);
    EXPECT_EQ(missing.code, PlatformError::NotFound);
    EXPECT_TRUE(loaded.isEmpty());
}
#endif
