#include <gtest/gtest.h>

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUuid>

#include "ConnectionStore.h"
#include "CurlWebDavProvider.h"
#include "FileOperations.h"
#include "GvfsMounter.h"
#include "LocalFileProvider.h"
#include "OperationQueue.h"

namespace {

constexpr qint64 kMiB = 1024 * 1024;
constexpr int kQueueTimeoutMs = 120000;

struct LiveConfig {
    QString host;
    int port = 0;
    QString parent;
    QString user;
    QString password;
    bool https = false;
    bool anonymous = false;
};

QString requiredEnvironment(const char *name, QString *error) {
    if (!qEnvironmentVariableIsSet(name) || qEnvironmentVariable(name).isEmpty()) {
        *error = QStringLiteral("Required environment variable %1 is not set").arg(
            QString::fromLatin1(name));
        return {};
    }
    return qEnvironmentVariable(name);
}

bool validateParent(const QString &path, QString *error) {
    if (!path.startsWith(QLatin1Char('/')) || path == QStringLiteral("/")) {
        *error = QStringLiteral("FC_WEBDAV_PARENT must be a non-root absolute provider path");
        return false;
    }
    if (path.contains(QLatin1Char('\\'))) {
        *error = QStringLiteral("FC_WEBDAV_PARENT must use provider '/' separators only");
        return false;
    }
    for (const QChar ch : path) {
        if (ch.category() == QChar::Other_Control) {
            *error = QStringLiteral("FC_WEBDAV_PARENT must not contain control characters");
            return false;
        }
    }
    for (const QString &part : path.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (part == QStringLiteral(".") || part == QStringLiteral("..")) {
            *error = QStringLiteral("FC_WEBDAV_PARENT must not contain traversal segments");
            return false;
        }
    }
    return true;
}

void clearLiveEnvironment() {
    static const char *const names[] = {
        "FC_WEBDAV_E2E", "FC_WEBDAV_CONNECTION_ID", "FC_WEBDAV_HOST",
        "FC_WEBDAV_PORT", "FC_WEBDAV_PARENT", "FC_WEBDAV_USER", "FC_WEBDAV_PASSWORD",
        "FC_WEBDAV_HTTPS", "FC_WEBDAV_ANONYMOUS",
    };
    for (const char *name : names)
        qunsetenv(name);
}

bool loadConfig(LiveConfig *config, QString *error) {
    const QString connectionId = qEnvironmentVariable("FC_WEBDAV_CONNECTION_ID");
    config->parent = requiredEnvironment("FC_WEBDAV_PARENT", error);
    if (!connectionId.isEmpty() && error->isEmpty()) {
        const SavedConnection saved = ConnectionStore::load(connectionId);
        if (saved.id.isEmpty()) {
            *error = QStringLiteral("FC_WEBDAV_CONNECTION_ID does not name a saved connection");
        } else if (saved.protocol != static_cast<int>(GvfsMounter::Protocol::WebDav) &&
                   saved.protocol != static_cast<int>(GvfsMounter::Protocol::WebDavs)) {
            *error = QStringLiteral("FC_WEBDAV_CONNECTION_ID must name a WebDAV connection");
        } else if (saved.host.isEmpty() || saved.port < 1 || saved.port > 65535) {
            *error = QStringLiteral("The saved WebDAV connection has invalid endpoint data");
        } else {
            config->host = saved.host;
            config->port = saved.port;
            config->user = saved.user;
            config->https = saved.protocol == static_cast<int>(GvfsMounter::Protocol::WebDavs);
            config->anonymous = saved.anonymous;
            const QString endpointPath = saved.remotePath;
            if (!endpointPath.isEmpty() && endpointPath != QStringLiteral("/") &&
                config->parent != endpointPath &&
                !config->parent.startsWith(endpointPath + QLatin1Char('/'))) {
                config->parent = endpointPath.endsWith(QLatin1Char('/'))
                                     ? endpointPath.chopped(1) + config->parent
                                     : endpointPath + config->parent;
            }
            if (!config->anonymous) {
                config->password = ConnectionStore::loadPassword(saved.id);
                if (config->password.isEmpty())
                    *error = QStringLiteral("The saved WebDAV connection has no password in the keyring");
            }
        }
    } else if (connectionId.isEmpty() && error->isEmpty()) {
        config->host = requiredEnvironment("FC_WEBDAV_HOST", error);
        if (error->isEmpty()) {
            bool ok = false;
            const int port = requiredEnvironment("FC_WEBDAV_PORT", error).toInt(&ok);
            if (error->isEmpty() && (!ok || port < 1 || port > 65535))
                *error = QStringLiteral("FC_WEBDAV_PORT must be between 1 and 65535");
            config->port = port;
        }
        const QString https = qEnvironmentVariable("FC_WEBDAV_HTTPS", QStringLiteral("0"));
        if (https != QStringLiteral("0") && https != QStringLiteral("1"))
            *error = QStringLiteral("FC_WEBDAV_HTTPS must be 0 or 1");
        config->https = https == QStringLiteral("1");
        config->anonymous = qEnvironmentVariable("FC_WEBDAV_ANONYMOUS") == QStringLiteral("1");
        if (!config->anonymous && error->isEmpty())
            config->user = requiredEnvironment("FC_WEBDAV_USER", error);
        if (!config->anonymous && error->isEmpty())
            config->password = requiredEnvironment("FC_WEBDAV_PASSWORD", error);
    }

    clearLiveEnvironment();
    return error->isEmpty() && validateParent(config->parent, error);
}

QString joinPath(const QString &dir, const QString &name) {
    return dir.endsWith(QLatin1Char('/')) ? dir + name : dir + QLatin1Char('/') + name;
}

bool writePatternFile(const QString &path, qint64 bytes, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *error = QStringLiteral("Could not create local fixture");
        return false;
    }
    QByteArray block(static_cast<int>(kMiB), Qt::Uninitialized);
    for (int i = 0; i < block.size(); ++i)
        block[i] = static_cast<char>((i * 29 + i / 251) % 251);
    while (bytes > 0) {
        const qint64 count = qMin<qint64>(bytes, block.size());
        if (file.write(block.constData(), count) != count) {
            *error = QStringLiteral("Could not write local fixture");
            return false;
        }
        bytes -= count;
    }
    return file.flush();
}

QByteArray sha256(const QString &path, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Could not open local file for checksum");
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buffer(static_cast<int>(kMiB), Qt::Uninitialized);
    while (true) {
        const qint64 count = file.read(buffer.data(), buffer.size());
        if (count < 0) {
            *error = QStringLiteral("Could not read local file for checksum");
            return {};
        }
        if (count == 0)
            break;
        hash.addData(buffer.constData(), count);
    }
    return hash.result();
}

bool connectProvider(CurlWebDavProvider *dav, const LiveConfig &config, QString *error) {
    dav->setTimeoutMs(20000);
    return dav->connectToHost(config.host, config.port, config.user, config.password, config.https,
                              error);
}

bool waitForJobs(QSignalSpy *finished, QSignalSpy *errors, int count, QString *error) {
    QElapsedTimer timer;
    timer.start();
    while (finished->count() < count) {
        const int remaining = kQueueTimeoutMs - static_cast<int>(timer.elapsed());
        if (remaining <= 0 || !finished->wait(remaining)) {
            *error = QStringLiteral("Timed out waiting for OperationQueue");
            return false;
        }
    }
    for (int i = 0; i < count; ++i) {
        if (!finished->at(i).at(0).toBool()) {
            *error = errors->isEmpty() ? QStringLiteral("OperationQueue reported a failed job")
                                       : errors->first().at(0).toString();
            return false;
        }
    }
    return true;
}

bool cleanupRoot(CurlWebDavProvider *dav, const LiveConfig &config, const QString &root,
                 QString *error) {
    QString reconnectError;
    if (!dav->reconnect(&reconnectError) && !connectProvider(dav, config, &reconnectError)) {
        *error = reconnectError.isEmpty() ? QStringLiteral("Could not reconnect for cleanup")
                                          : reconnectError;
        return false;
    }
    if (!dav->exists(root))
        return true;

    FileOperations cleanup;
    cleanup.setErrorResolver([](const QString &, const QString &) { return ErrorAction::Cancel; });
    QString firstError;
    if (cleanup.deleteProviderPaths(dav, {root}, &firstError) && !dav->exists(root))
        return true;

    if (!dav->reconnect(&reconnectError)) {
        *error = reconnectError.isEmpty() ? firstError : reconnectError;
        return false;
    }
    if (!dav->exists(root))
        return true;
    QString retryError;
    if (!cleanup.deleteProviderPaths(dav, {root}, &retryError) || dav->exists(root)) {
        *error = retryError.isEmpty() ? firstError : retryError;
        return false;
    }
    return true;
}

bool runScenario(CurlWebDavProvider *dav, const QString &root, const QString &source,
                 const QString &prefix, const QString &downloadDir, QString *error) {
    auto *local = LocalFileProvider::instance();
    const ConflictResolver overwrite = [](const FileConflict &) { return ErrorAction::Overwrite; };
    FileOperations ops;
    ops.setErrorResolver([](const QString &, const QString &) { return ErrorAction::Cancel; });

    if (!ops.copyAcrossProviders(local, {prefix}, dav, root, false, overwrite, error))
        return false;
    QString reconnectError;
    if (!dav->reconnect(&reconnectError)) {
        *error = reconnectError.isEmpty() ? QStringLiteral("WebDAV reconnect failed") : reconnectError;
        return false;
    }
    // The remote file is deliberately shorter. WebDAV must replace it with a
    // full PUT rather than asking its non-append write handle to seek.
    if (!ops.copyAcrossProviders(local, {source}, dav, root, false, overwrite, error))
        return false;

    const QString remoteFile = joinPath(root, QFileInfo(source).fileName());
    if (!ops.copyAcrossProviders(dav, {remoteFile}, local, downloadDir, false, overwrite, error))
        return false;
    const QString downloaded = QDir(downloadDir).filePath(QFileInfo(source).fileName());
    QString hashError;
    if (sha256(source, &hashError) != sha256(downloaded, &hashError) ||
        QFileInfo(source).size() != QFileInfo(downloaded).size()) {
        *error = hashError.isEmpty() ? QStringLiteral("WebDAV upload/download hash mismatch") : hashError;
        return false;
    }

    if (!ops.makeProviderDirectory(dav, root, QStringLiteral("moved"), error))
        return false;
    const QString movedDir = joinPath(root, QStringLiteral("moved"));
    if (!ops.copyAcrossProviders(dav, {remoteFile}, dav, movedDir, true, overwrite, error))
        return false;
    const QString movedFile = joinPath(movedDir, QFileInfo(source).fileName());
    if (dav->exists(remoteFile) || !dav->exists(movedFile)) {
        *error = QStringLiteral("WebDAV server-side move produced the wrong remote state");
        return false;
    }

    const QString queued = QDir(QFileInfo(source).dir()).filePath(QStringLiteral("queued.bin"));
    if (!writePatternFile(queued, 512 * 1024, error))
        return false;
    OperationQueue queue;
    queue.setMaxConcurrentTransfers(1);
    queue.setConflictHandler(overwrite);
    queue.setErrorHandler([](const QString &, const QString &) { return ErrorAction::Cancel; });
    QSignalSpy finished(&queue, &OperationQueue::finished);
    QSignalSpy errors(&queue, &OperationQueue::errorOccurred);
    queue.enqueueProviderMkdir(dav, root, QStringLiteral("queued"));
    queue.enqueueProviderCopy(local, {queued}, dav, joinPath(root, QStringLiteral("queued")));
    if (!waitForJobs(&finished, &errors, 2, error))
        return false;
    const QString queuedDir = joinPath(root, QStringLiteral("queued"));
    const QString queuedFile = joinPath(queuedDir, QFileInfo(queued).fileName());
    if (!dav->exists(queuedFile)) {
        *error = QStringLiteral("Queued WebDAV copy did not create its remote file");
        return false;
    }

    OperationQueue moveQueue;
    moveQueue.setMaxConcurrentTransfers(1);
    moveQueue.setConflictHandler(overwrite);
    moveQueue.setErrorHandler([](const QString &, const QString &) { return ErrorAction::Cancel; });
    QSignalSpy moveFinished(&moveQueue, &OperationQueue::finished);
    QSignalSpy moveErrors(&moveQueue, &OperationQueue::errorOccurred);
    moveQueue.enqueueProviderMove(dav, {queuedFile}, dav, movedDir);
    if (!waitForJobs(&moveFinished, &moveErrors, 1, error))
        return false;
    const QString movedQueuedFile = joinPath(movedDir, QFileInfo(queued).fileName());
    if (dav->exists(queuedFile) || !dav->exists(movedQueuedFile)) {
        *error = QStringLiteral("Queued WebDAV move produced the wrong remote state");
        return false;
    }

    OperationQueue renameQueue;
    renameQueue.setMaxConcurrentTransfers(1);
    renameQueue.setErrorHandler([](const QString &, const QString &) { return ErrorAction::Cancel; });
    QSignalSpy renameFinished(&renameQueue, &OperationQueue::finished);
    QSignalSpy renameErrors(&renameQueue, &OperationQueue::errorOccurred);
    const QString renamedName = QStringLiteral("queued-renamed.bin");
    renameQueue.enqueueProviderRename(dav, movedQueuedFile, renamedName);
    if (!waitForJobs(&renameFinished, &renameErrors, 1, error))
        return false;
    const QString renamedQueuedFile = joinPath(movedDir, renamedName);
    if (dav->exists(movedQueuedFile) || !dav->exists(renamedQueuedFile)) {
        *error = QStringLiteral("Queued WebDAV rename produced the wrong remote state");
        return false;
    }

    OperationQueue downloadQueue;
    downloadQueue.setMaxConcurrentTransfers(1);
    downloadQueue.setConflictHandler(overwrite);
    downloadQueue.setErrorHandler([](const QString &, const QString &) { return ErrorAction::Cancel; });
    QSignalSpy downloadFinished(&downloadQueue, &OperationQueue::finished);
    QSignalSpy downloadErrors(&downloadQueue, &OperationQueue::errorOccurred);
    downloadQueue.enqueueProviderCopy(dav, {renamedQueuedFile}, local, downloadDir);
    if (!waitForJobs(&downloadFinished, &downloadErrors, 1, error))
        return false;
    const QString downloadedQueued = QDir(downloadDir).filePath(renamedName);
    QString queuedHashError;
    if (sha256(queued, &queuedHashError) != sha256(downloadedQueued, &queuedHashError) ||
        QFileInfo(queued).size() != QFileInfo(downloadedQueued).size()) {
        *error = queuedHashError.isEmpty()
                     ? QStringLiteral("Queued WebDAV download hash mismatch")
                     : queuedHashError;
        return false;
    }

    OperationQueue deleteQueue;
    deleteQueue.setMaxConcurrentTransfers(1);
    deleteQueue.setErrorHandler([](const QString &, const QString &) { return ErrorAction::Cancel; });
    QSignalSpy deleteFinished(&deleteQueue, &OperationQueue::finished);
    QSignalSpy deleteErrors(&deleteQueue, &OperationQueue::errorOccurred);
    deleteQueue.enqueueProviderDelete(dav, {movedFile, renamedQueuedFile});
    if (!waitForJobs(&deleteFinished, &deleteErrors, 1, error))
        return false;
    if (dav->exists(movedFile) || dav->exists(renamedQueuedFile)) {
        *error = QStringLiteral("Queued WebDAV delete left test files behind");
        return false;
    }
    return true;
}

} // namespace

TEST(WebDavLiveConfig, RejectsUnsafeParent) {
    QString error;
    EXPECT_FALSE(validateParent(QStringLiteral("/"), &error));
    EXPECT_FALSE(validateParent(QStringLiteral("/dav/../outside"), &error));
    EXPECT_FALSE(validateParent(QStringLiteral("dav/no-leading-slash"), &error));
    EXPECT_TRUE(validateParent(QStringLiteral("/dav/test-parent"), &error));
}

TEST(WebDavLiveTest, ApplicationOperationsRoundTripMoveQueueReconnectAndCleanup) {
    if (qEnvironmentVariable("FC_WEBDAV_E2E") != QStringLiteral("1"))
        GTEST_SKIP() << "Set FC_WEBDAV_E2E=1 to enable the live application-internal WebDAV test";

    LiveConfig config;
    QString configError;
    ASSERT_TRUE(loadConfig(&config, &configError)) << configError.toStdString();

    CurlWebDavProvider dav;
    QString connectError;
    ASSERT_TRUE(connectProvider(&dav, config, &connectError)) << connectError.toStdString();
    const QString parent = dav.cleanPath(config.parent);
    ASSERT_TRUE(dav.exists(parent) && dav.isDir(parent))
        << "Configured WebDAV parent is not an existing collection";

    QTemporaryDir localRoot;
    ASSERT_TRUE(localRoot.isValid());
    const QString sourceDir = QDir(localRoot.path()).filePath(QStringLiteral("source"));
    const QString prefixDir = QDir(localRoot.path()).filePath(QStringLiteral("prefix"));
    const QString downloadDir = QDir(localRoot.path()).filePath(QStringLiteral("download"));
    ASSERT_TRUE(QDir().mkpath(sourceDir) && QDir().mkpath(prefixDir) && QDir().mkpath(downloadDir));
    const QString source = QDir(sourceDir).filePath(QStringLiteral("payload.bin"));
    const QString prefix = QDir(prefixDir).filePath(QStringLiteral("payload.bin"));
    QString localError;
    ASSERT_TRUE(writePatternFile(source, 8 * kMiB, &localError)) << localError.toStdString();
    ASSERT_TRUE(writePatternFile(prefix, 2 * kMiB, &localError)) << localError.toStdString();

    const QString root = joinPath(parent, QStringLiteral("FC-WEBDAV-E2E-") +
                                      QUuid::createUuid().toString(QUuid::WithoutBraces));
    FileOperations setup;
    QString setupError;
    const bool setupOk = setup.makeProviderDirectory(&dav, parent, QFileInfo(root).fileName(),
                                                      &setupError);
    QString scenarioError;
    const bool scenarioOk = setupOk && runScenario(&dav, root, source, prefix, downloadDir,
                                                    &scenarioError);
    QString cleanupError;
    const bool cleanupOk = cleanupRoot(&dav, config, root, &cleanupError);

    EXPECT_TRUE(setupOk) << setupError.toStdString();
    EXPECT_TRUE(scenarioOk) << scenarioError.toStdString();
    EXPECT_TRUE(cleanupOk) << cleanupError.toStdString();
}
