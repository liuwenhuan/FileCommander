#include <gtest/gtest.h>

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUuid>

#include "ConnectionStore.h"
#include "FileOperations.h"
#include "GvfsMounter.h"
#include "LocalFileProvider.h"
#include "OperationQueue.h"
#include "SmbProvider.h"

namespace {

constexpr qint64 kMiB = 1024 * 1024;
constexpr qint64 kFixtureBytes = 128 * kMiB;
constexpr qint64 kPartialBytes = 32 * kMiB;
constexpr int kQueueTimeoutMs = 120000;

struct LiveConfig {
    QString host;
    QString shareParent;
    QString user;
    QString password;
    QString workgroup;
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

bool validateShareParent(const QString &path, QString *error) {
    if (!path.startsWith(QLatin1Char('/'))) {
        *error = QStringLiteral("FC_SMB_PARENT must be an absolute provider path inside a share");
        return false;
    }
    if (path.contains(QLatin1Char('\\'))) {
        *error = QStringLiteral("FC_SMB_PARENT must use provider '/' separators only");
        return false;
    }
    for (const QChar ch : path) {
        if (ch.category() == QChar::Other_Control) {
            *error = QStringLiteral("FC_SMB_PARENT must not contain control characters");
            return false;
        }
    }

    const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        *error = QStringLiteral("FC_SMB_PARENT must name a concrete share");
        return false;
    }
    for (const QString &part : parts) {
        if (part == QStringLiteral(".") || part == QStringLiteral("..")) {
            *error = QStringLiteral("FC_SMB_PARENT must not contain traversal segments");
            return false;
        }
    }
    return true;
}

void clearLiveEnvironment() {
    static const char *const names[] = {
        "FC_SMB_CONNECTION_ID", "FC_SMB_HOST",       "FC_SMB_PARENT",
        "FC_SMB_USER",          "FC_SMB_PASSWORD",   "FC_SMB_WORKGROUP", "FC_SMB_ANONYMOUS",
    };
    for (const char *name : names)
        qunsetenv(name);
}

bool loadConfig(LiveConfig *config, QString *error) {
    const QString connectionId = qEnvironmentVariable("FC_SMB_CONNECTION_ID");
    config->shareParent = requiredEnvironment("FC_SMB_PARENT", error);
    if (!connectionId.isEmpty() && error->isEmpty()) {
        const SavedConnection saved = ConnectionStore::load(connectionId);
        if (saved.id.isEmpty()) {
            *error = QStringLiteral("FC_SMB_CONNECTION_ID does not name a saved connection");
        } else if (saved.protocol != static_cast<int>(GvfsMounter::Protocol::Smb)) {
            *error = QStringLiteral("FC_SMB_CONNECTION_ID must name an SMB connection");
        } else if (saved.host.isEmpty()) {
            *error = QStringLiteral("The saved SMB connection has no host");
        } else if (saved.port > 0 && saved.port != 445) {
            *error = QStringLiteral("The saved SMB connection must use port 445");
        } else {
            config->host = saved.host;
            config->user = saved.user;
            config->anonymous = saved.anonymous;
            if (!config->anonymous) {
                config->password = ConnectionStore::loadPassword(saved.id);
                if (config->password.isEmpty())
                    *error = QStringLiteral("The saved SMB connection has no password in the keyring");
            }
        }
    } else if (connectionId.isEmpty() && error->isEmpty()) {
        config->host = requiredEnvironment("FC_SMB_HOST", error);
        config->anonymous = qEnvironmentVariable("FC_SMB_ANONYMOUS") == QStringLiteral("1");
        if (!config->anonymous && error->isEmpty())
            config->user = requiredEnvironment("FC_SMB_USER", error);
        if (!config->anonymous && error->isEmpty())
            config->password = requiredEnvironment("FC_SMB_PASSWORD", error);
        config->workgroup = qEnvironmentVariable("FC_SMB_WORKGROUP");
    }

    clearLiveEnvironment();
    return error->isEmpty() && validateShareParent(config->shareParent, error);
}

bool writePatternFile(const QString &path, qint64 bytes, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *error = QStringLiteral("Could not create local fixture");
        return false;
    }

    QByteArray block;
    block.resize(static_cast<int>(kMiB));
    for (int i = 0; i < block.size(); ++i)
        block[i] = static_cast<char>((i * 17 + i / 251) % 251);

    qint64 remaining = bytes;
    while (remaining > 0) {
        const qint64 amount = qMin<qint64>(remaining, block.size());
        if (file.write(block.constData(), amount) != amount) {
            *error = QStringLiteral("Could not write local fixture");
            return false;
        }
        remaining -= amount;
    }
    if (!file.flush()) {
        *error = QStringLiteral("Could not flush local fixture");
        return false;
    }
    return true;
}

QByteArray fileSha256(const QString &path, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Could not open local file for checksum");
        return {};
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buffer;
    buffer.resize(static_cast<int>(kMiB));
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

QString joinProviderPath(const QString &dir, const QString &name) {
    return dir.endsWith(QLatin1Char('/')) ? dir + name : dir + QLatin1Char('/') + name;
}

bool waitForFinished(QSignalSpy *finished, int expected, QString *error) {
    QElapsedTimer timer;
    timer.start();
    while (finished->count() < expected) {
        const int remaining = kQueueTimeoutMs - static_cast<int>(timer.elapsed());
        if (remaining <= 0 || !finished->wait(remaining)) {
            *error = QStringLiteral("Timed out waiting for OperationQueue");
            return false;
        }
    }
    for (int i = 0; i < expected; ++i) {
        if (!finished->at(i).at(0).toBool()) {
            *error = QStringLiteral("OperationQueue reported a failed job");
            return false;
        }
    }
    return true;
}

QString firstQueueError(const QSignalSpy &errors, const QString &fallback) {
    return errors.isEmpty() ? fallback : errors.first().at(0).toString();
}

bool connectProvider(SmbProvider *provider, const LiveConfig &config, QString *error) {
    provider->setTimeoutMs(20000);
    return provider->connectToHost(config.host, config.user, config.password, config.workgroup,
                                   config.anonymous, error);
}

bool runScenario(SmbProvider *smb, const QString &remoteRoot, const QString &largeFixture,
                 const QString &partialFixture, const QString &queuedA, const QString &queuedB,
                 const QString &downloadDir, QString *error) {
    auto *local = LocalFileProvider::instance();
    const ConflictResolver overwrite = [](const FileConflict &) { return ErrorAction::Overwrite; };

    FileOperations ops;
    ops.setErrorResolver([](const QString &, const QString &) { return ErrorAction::Cancel; });

    if (!ops.copyAcrossProviders(local, {partialFixture}, smb, remoteRoot,
                                 /*removeSource=*/false, overwrite, error))
        return false;

    QString reconnectError;
    if (!smb->reconnect(&reconnectError)) {
        *error = reconnectError.isEmpty() ? QStringLiteral("SMB reconnect failed") : reconnectError;
        return false;
    }

    bool sawResumeOffset = false;
    const auto resumeProgress = QObject::connect(
        &ops, &FileOperations::progress,
        [&sawResumeOffset](qint64, qint64, qint64 doneBytes, qint64 totalBytes, const QString &) {
            if (doneBytes == kPartialBytes && totalBytes == kFixtureBytes)
                sawResumeOffset = true;
        });
    const bool resumed = ops.copyAcrossProviders(local, {largeFixture}, smb, remoteRoot,
                                                  /*removeSource=*/false, overwrite, error);
    QObject::disconnect(resumeProgress);
    if (!resumed)
        return false;
    if (!sawResumeOffset) {
        *error = QStringLiteral("The SMB upload did not report the 32 MiB resume point");
        return false;
    }

    const QString remoteLarge = joinProviderPath(remoteRoot, QFileInfo(largeFixture).fileName());
    if (!ops.copyAcrossProviders(smb, {remoteLarge}, local, downloadDir,
                                 /*removeSource=*/false, overwrite, error))
        return false;

    QString checksumError;
    const QByteArray sourceHash = fileSha256(largeFixture, &checksumError);
    if (!checksumError.isEmpty()) {
        *error = checksumError;
        return false;
    }
    const QString downloadedLarge =
        QDir(downloadDir).filePath(QFileInfo(largeFixture).fileName());
    const QByteArray downloadedHash = fileSha256(downloadedLarge, &checksumError);
    if (!checksumError.isEmpty()) {
        *error = checksumError;
        return false;
    }
    if (QFileInfo(downloadedLarge).size() != kFixtureBytes || sourceHash != downloadedHash) {
        *error = QStringLiteral("The 128 MiB SMB round trip changed the file");
        return false;
    }

    const QString movedName = QStringLiteral("moved");
    if (!ops.makeProviderDirectory(smb, remoteRoot, movedName, error))
        return false;
    const QString movedDir = joinProviderPath(remoteRoot, movedName);
    int moveProgressSignals = 0;
    const auto moveProgress = QObject::connect(
        &ops, &FileOperations::progress,
        [&moveProgressSignals](qint64, qint64, qint64, qint64, const QString &) {
            ++moveProgressSignals;
        });
    const bool moved = ops.copyAcrossProviders(smb, {remoteLarge}, smb, movedDir,
                                                /*removeSource=*/true, overwrite, error);
    QObject::disconnect(moveProgress);
    if (!moved)
        return false;
    if (moveProgressSignals != 1) {
        *error = QStringLiteral("The same-share SMB move did not use the server-side fast path");
        return false;
    }
    const QString movedLarge = joinProviderPath(movedDir, QFileInfo(largeFixture).fileName());
    if (!smb->exists(movedLarge) || smb->exists(remoteLarge)) {
        *error = QStringLiteral("The same-share server-side move produced the wrong remote state");
        return false;
    }

    OperationQueue uploads;
    uploads.setMaxConcurrentTransfers(2);
    uploads.setConflictHandler([](const FileConflict &) { return ErrorAction::Overwrite; });
    uploads.setErrorHandler([](const QString &, const QString &) { return ErrorAction::Cancel; });
    QSignalSpy uploadFinished(&uploads, &OperationQueue::finished);
    QSignalSpy uploadErrors(&uploads, &OperationQueue::errorOccurred);
    uploads.enqueueProviderCopy(local, {queuedA}, smb, remoteRoot);
    uploads.enqueueProviderCopy(local, {queuedB}, smb, remoteRoot);
    if (!waitForFinished(&uploadFinished, 2, error)) {
        *error = firstQueueError(uploadErrors, *error);
        return false;
    }

    const QString remoteA = joinProviderPath(remoteRoot, QFileInfo(queuedA).fileName());
    const QString remoteB = joinProviderPath(remoteRoot, QFileInfo(queuedB).fileName());
    if (!smb->exists(remoteA) || !smb->exists(remoteB)) {
        *error = QStringLiteral("Queued SMB uploads did not create both remote files");
        return false;
    }

    OperationQueue renameQueue;
    renameQueue.setMaxConcurrentTransfers(1);
    renameQueue.setErrorHandler([](const QString &, const QString &) { return ErrorAction::Cancel; });
    QSignalSpy renameFinished(&renameQueue, &OperationQueue::finished);
    QSignalSpy renameErrors(&renameQueue, &OperationQueue::errorOccurred);
    const QString renamedAName = QStringLiteral("queued-a-renamed.bin");
    renameQueue.enqueueProviderRename(smb, remoteA, renamedAName);
    if (!waitForFinished(&renameFinished, 1, error)) {
        *error = firstQueueError(renameErrors, *error);
        return false;
    }
    const QString renamedA = joinProviderPath(remoteRoot, renamedAName);
    if (smb->exists(remoteA) || !smb->exists(renamedA)) {
        *error = QStringLiteral("Queued SMB rename produced the wrong remote state");
        return false;
    }

    OperationQueue downloads;
    downloads.setMaxConcurrentTransfers(1);
    downloads.setConflictHandler([](const FileConflict &) { return ErrorAction::Overwrite; });
    downloads.setErrorHandler([](const QString &, const QString &) { return ErrorAction::Cancel; });
    QSignalSpy downloadFinished(&downloads, &OperationQueue::finished);
    QSignalSpy downloadErrors(&downloads, &OperationQueue::errorOccurred);
    downloads.enqueueProviderCopy(smb, {renamedA}, local, downloadDir);
    if (!waitForFinished(&downloadFinished, 1, error)) {
        *error = firstQueueError(downloadErrors, *error);
        return false;
    }
    QString queuedChecksumError;
    const QString downloadedA = QDir(downloadDir).filePath(renamedAName);
    if (QFileInfo(downloadedA).size() != QFileInfo(queuedA).size() ||
        fileSha256(queuedA, &queuedChecksumError) != fileSha256(downloadedA, &queuedChecksumError)) {
        *error = queuedChecksumError.isEmpty() ? QStringLiteral("Queued SMB download hash mismatch")
                                                : queuedChecksumError;
        return false;
    }

    OperationQueue removals;
    removals.setMaxConcurrentTransfers(2);
    removals.setErrorHandler([](const QString &, const QString &) { return ErrorAction::Cancel; });
    QSignalSpy deleteFinished(&removals, &OperationQueue::finished);
    QSignalSpy deleteErrors(&removals, &OperationQueue::errorOccurred);
    removals.enqueueProviderDelete(smb, {movedLarge, renamedA, remoteB});
    if (!waitForFinished(&deleteFinished, 1, error)) {
        *error = firstQueueError(deleteErrors, *error);
        return false;
    }
    if (smb->exists(movedLarge) || smb->exists(renamedA) || smb->exists(remoteB)) {
        *error = QStringLiteral("Queued SMB delete left one or more test files behind");
        return false;
    }

    return true;
}

bool cleanupRemoteRoot(SmbProvider *smb, const LiveConfig &config, const QString &remoteRoot,
                       QString *error) {
    QString connectError;
    if (!smb->reconnect(&connectError) && !connectProvider(smb, config, &connectError)) {
        *error = connectError.isEmpty() ? QStringLiteral("Could not reconnect for cleanup")
                                        : connectError;
        return false;
    }

    if (!smb->exists(remoteRoot))
        return true;

    FileOperations cleanup;
    cleanup.setErrorResolver([](const QString &, const QString &) { return ErrorAction::Cancel; });
    QString firstError;
    const bool firstDelete = cleanup.deleteProviderPaths(smb, {remoteRoot}, &firstError);
    if (firstDelete && !smb->exists(remoteRoot))
        return true;

    if (firstError.isEmpty())
        firstError = QStringLiteral("The session-created SMB test directory still exists");

    QString reconnectError;
    if (!smb->reconnect(&reconnectError)) {
        *error = reconnectError.isEmpty() ? firstError : reconnectError;
        return false;
    }

    if (!smb->exists(remoteRoot))
        return true;

    QString retryError;
    if (!cleanup.deleteProviderPaths(smb, {remoteRoot}, &retryError)) {
        *error = retryError.isEmpty() ? firstError : retryError;
        return false;
    }

    if (!smb->exists(remoteRoot))
        return true;
    *error = QStringLiteral("The session-created SMB test directory still exists");
    return false;
}

} // namespace

TEST(SmbLiveConfig, AcceptsAConcreteShareParent) {
    QString error;
    EXPECT_TRUE(validateShareParent(QStringLiteral("/share/test-parent"), &error));
    EXPECT_TRUE(error.isEmpty());
}

TEST(SmbLiveConfig, RejectsTraversalAndServerRootParents) {
    const QStringList invalid = {
        QStringLiteral("/"),
        QStringLiteral("/share/.."),
        QStringLiteral("/share/../../other-share"),
        QStringLiteral("/share/./nested"),
        QStringLiteral("/share/safe\\..\\outside"),
        QStringLiteral("/share/control\nsegment"),
    };
    for (const QString &path : invalid) {
        QString error;
        EXPECT_FALSE(validateShareParent(path, &error)) << path.toStdString();
        EXPECT_FALSE(error.isEmpty()) << path.toStdString();
    }
}

TEST(SmbLiveTest, NoSpaceWriteReportsSpecificError) {
    if (qEnvironmentVariable("FC_SMB_E2E_NOSPACE") != QStringLiteral("1"))
        GTEST_SKIP() << "Set FC_SMB_E2E_NOSPACE=1 to verify a full SMB destination";

    LiveConfig config;
    QString configError;
    ASSERT_TRUE(loadConfig(&config, &configError)) << configError.toStdString();

    QTemporaryDir localRoot;
    ASSERT_TRUE(localRoot.isValid());
    const QString fixture = QDir(localRoot.path()).filePath(QStringLiteral("no-space.bin"));
    QString localError;
    ASSERT_TRUE(writePatternFile(fixture, kMiB, &localError)) << localError.toStdString();

    SmbProvider smb;
    QString connectError;
    ASSERT_TRUE(connectProvider(&smb, config, &connectError)) << connectError.toStdString();

    const QString parent = smb.cleanPath(config.shareParent);
    ASSERT_TRUE(smb.exists(parent) && smb.isDir(parent))
        << "Configured SMB parent is not an existing directory";
    const QString rootName = QStringLiteral("FC-SMB-E2E-") +
                             QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString remoteRoot = joinProviderPath(parent, rootName);

    FileOperations setup;
    QString setupError;
    if (!setup.makeProviderDirectory(&smb, parent, rootName, &setupError)) {
        QString cleanupError;
        const bool cleanupOk = cleanupRemoteRoot(&smb, config, remoteRoot, &cleanupError);
        ADD_FAILURE() << setupError.toStdString();
        EXPECT_TRUE(cleanupOk)
            << (QStringLiteral("Cleanup after setup failure failed for ") + remoteRoot +
                QStringLiteral(": ") + cleanupError)
                   .toStdString();
        return;
    }

    FileOperations ops;
    ops.setErrorResolver([](const QString &, const QString &) { return ErrorAction::Cancel; });
    QSignalSpy errors(&ops, &FileOperations::errorOccurred);
    QString uploadError;
    const bool uploaded = ops.copyAcrossProviders(LocalFileProvider::instance(), {fixture}, &smb,
                                                   remoteRoot, /*removeSource=*/false, nullptr,
                                                   &uploadError);

    QString cleanupError;
    const bool cleanupOk = cleanupRemoteRoot(&smb, config, remoteRoot, &cleanupError);

    const QString remotePath = joinProviderPath(remoteRoot, QFileInfo(fixture).fileName());
    const QString expected = QStringLiteral("The destination has no space for %1").arg(remotePath);
    EXPECT_FALSE(uploaded);
    if (!uploaded) {
        EXPECT_EQ(uploadError, expected);
        ASSERT_EQ(errors.count(), 1);
        EXPECT_EQ(errors.first().at(0).toString(), expected);
    }
    EXPECT_TRUE(cleanupOk) << (QStringLiteral("Cleanup failed for ") + remoteRoot +
                               QStringLiteral(": ") + cleanupError)
                                  .toStdString();
}

TEST(SmbLiveTest, ApplicationOperationsRoundTripMoveQueueReconnectAndCleanup) {
    if (qEnvironmentVariable("FC_SMB_E2E") != QStringLiteral("1"))
        GTEST_SKIP() << "Set FC_SMB_E2E=1 to enable the live application-internal SMB test";

    LiveConfig config;
    QString configError;
    ASSERT_TRUE(loadConfig(&config, &configError)) << configError.toStdString();

    QTemporaryDir localRoot;
    ASSERT_TRUE(localRoot.isValid());
    const QString sourceDir = QDir(localRoot.path()).filePath(QStringLiteral("source"));
    const QString partialDir = QDir(localRoot.path()).filePath(QStringLiteral("partial"));
    const QString downloadDir = QDir(localRoot.path()).filePath(QStringLiteral("download"));
    ASSERT_TRUE(QDir().mkpath(sourceDir));
    ASSERT_TRUE(QDir().mkpath(partialDir));
    ASSERT_TRUE(QDir().mkpath(downloadDir));

    const QString largeFixture = QDir(sourceDir).filePath(QStringLiteral("payload.bin"));
    const QString partialFixture = QDir(partialDir).filePath(QStringLiteral("payload.bin"));
    const QString queuedA = QDir(sourceDir).filePath(QStringLiteral("queued-a.bin"));
    const QString queuedB = QDir(sourceDir).filePath(QStringLiteral("queued-b.bin"));
    QString localError;
    ASSERT_TRUE(writePatternFile(largeFixture, kFixtureBytes, &localError))
        << localError.toStdString();
    ASSERT_TRUE(writePatternFile(partialFixture, kPartialBytes, &localError))
        << localError.toStdString();
    ASSERT_TRUE(writePatternFile(queuedA, 2 * kMiB, &localError)) << localError.toStdString();
    ASSERT_TRUE(writePatternFile(queuedB, 3 * kMiB, &localError)) << localError.toStdString();

    SmbProvider smb;
    QString connectError;
    ASSERT_TRUE(connectProvider(&smb, config, &connectError)) << connectError.toStdString();

    const QString parent = smb.cleanPath(config.shareParent);
    ASSERT_TRUE(smb.exists(parent) && smb.isDir(parent))
        << "Configured SMB parent is not an existing directory";
    const QString runId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString rootName = QStringLiteral("FC-SMB-E2E-") + runId;
    const QString remoteRoot = joinProviderPath(parent, rootName);

    FileOperations setup;
    QString setupError;
    if (!setup.makeProviderDirectory(&smb, parent, rootName, &setupError)) {
        QString cleanupError;
        const bool cleanupOk = cleanupRemoteRoot(&smb, config, remoteRoot, &cleanupError);
        ADD_FAILURE() << setupError.toStdString();
        EXPECT_TRUE(cleanupOk)
            << (QStringLiteral("Cleanup after setup failure failed for ") + remoteRoot +
                QStringLiteral(": ") + cleanupError)
                   .toStdString();
        return;
    }

    QString scenarioError;
    const bool scenarioOk = runScenario(&smb, remoteRoot, largeFixture, partialFixture, queuedA,
                                        queuedB, downloadDir, &scenarioError);

    QString cleanupError;
    const bool cleanupOk = cleanupRemoteRoot(&smb, config, remoteRoot, &cleanupError);

    EXPECT_TRUE(scenarioOk) << scenarioError.toStdString();
    EXPECT_TRUE(cleanupOk) << (QStringLiteral("Cleanup failed for ") + remoteRoot +
                               QStringLiteral(": ") + cleanupError)
                                  .toStdString();
}
