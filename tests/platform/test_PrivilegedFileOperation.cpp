#include <gtest/gtest.h>

#include "privilege/PrivilegeBroker.h"
#include "privilege/PrivilegedFileOperation.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

namespace {

QByteArray encodedJson(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact).toBase64();
}

PrivilegeResult decode(const QJsonObject &object, PrivilegedOperationRequest *request = nullptr)
{
    return decodePrivilegedRequest(encodedJson(object), request);
}

QJsonObject validCopyJson()
{
    return {
        {QStringLiteral("version"), 1},
        {QStringLiteral("kind"), QStringLiteral("Copy")},
        {QStringLiteral("sourcePath"), QDir::temp().filePath(QStringLiteral("source.txt"))},
        {QStringLiteral("targetPath"), QDir::temp().filePath(QStringLiteral("target.txt"))},
        {QStringLiteral("overwrite"), false},
    };
}

} // namespace

TEST(PrivilegedFileOperationTest, ValidAbsoluteLocalCopyRoundTrips)
{
    const PrivilegedOperationRequest request{
        1,
        PrivilegedOperationKind::Copy,
        QDir::temp().filePath(QStringLiteral("source.txt")),
        QDir::temp().filePath(QStringLiteral("target.txt")),
        true,
    };

    PrivilegedOperationRequest decoded;
    const PrivilegeResult result = decodePrivilegedRequest(encodePrivilegedRequest(request), &decoded);

    EXPECT_EQ(result.status, PrivilegeStatus::Succeeded);
    EXPECT_EQ(decoded.version, request.version);
    EXPECT_EQ(decoded.kind, request.kind);
    EXPECT_EQ(decoded.sourcePath, request.sourcePath);
    EXPECT_EQ(decoded.targetPath, request.targetPath);
    EXPECT_EQ(decoded.overwrite, request.overwrite);
}

TEST(PrivilegedFileOperationTest, RejectsRelativePathsWithoutNormalizingThem)
{
    QJsonObject request = validCopyJson();
    request.insert(QStringLiteral("sourcePath"), QStringLiteral("relative/source.txt"));

    EXPECT_EQ(decode(request).status, PrivilegeStatus::InvalidRequest);
}

TEST(PrivilegedFileOperationTest, RejectsRemoteUrlPaths)
{
    QJsonObject httpsRequest = validCopyJson();
    httpsRequest.insert(QStringLiteral("sourcePath"), QStringLiteral("https://example.test/source.txt"));
    EXPECT_EQ(decode(httpsRequest).status, PrivilegeStatus::InvalidRequest);

    QJsonObject sftpRequest = validCopyJson();
    sftpRequest.insert(QStringLiteral("targetPath"), QStringLiteral("sftp://example.test/target.txt"));
    EXPECT_EQ(decode(sftpRequest).status, PrivilegeStatus::InvalidRequest);
}

TEST(PrivilegedFileOperationTest, RejectsUncPaths)
{
    QJsonObject slashRequest = validCopyJson();
    slashRequest.insert(QStringLiteral("sourcePath"), QStringLiteral("//server/share/source.txt"));
    EXPECT_EQ(decode(slashRequest).status, PrivilegeStatus::InvalidRequest);

    QJsonObject backslashRequest = validCopyJson();
    backslashRequest.insert(QStringLiteral("targetPath"), QStringLiteral("\\\\server\\share\\target.txt"));
    EXPECT_EQ(decode(backslashRequest).status, PrivilegeStatus::InvalidRequest);
}

TEST(PrivilegedFileOperationTest, RejectsUnknownJsonFields)
{
    QJsonObject request = validCopyJson();
    request.insert(QStringLiteral("unexpected"), true);

    EXPECT_EQ(decode(request).status, PrivilegeStatus::InvalidRequest);
}

TEST(PrivilegedFileOperationTest, RejectsUnknownVersionAndKind)
{
    QJsonObject unknownVersion = validCopyJson();
    unknownVersion.insert(QStringLiteral("version"), 2);
    EXPECT_EQ(decode(unknownVersion).status, PrivilegeStatus::InvalidRequest);

    QJsonObject fractionalVersion = validCopyJson();
    fractionalVersion.insert(QStringLiteral("version"), 1.5);
    EXPECT_EQ(decode(fractionalVersion).status, PrivilegeStatus::InvalidRequest);

    QJsonObject unknownKind = validCopyJson();
    unknownKind.insert(QStringLiteral("kind"), QStringLiteral("Archive"));
    EXPECT_EQ(decode(unknownKind).status, PrivilegeStatus::InvalidRequest);
}

TEST(PrivilegedFileOperationTest, RejectsMissingTargetForCopy)
{
    QJsonObject request = validCopyJson();
    request.remove(QStringLiteral("targetPath"));

    EXPECT_EQ(decode(request).status, PrivilegeStatus::InvalidRequest);
}

TEST(PrivilegedFileOperationTest, RejectsTrashDelete)
{
    QJsonObject request = validCopyJson();
    request.insert(QStringLiteral("kind"), QStringLiteral("Trash"));
    request.remove(QStringLiteral("targetPath"));

    EXPECT_EQ(decode(request).status, PrivilegeStatus::InvalidRequest);
}

TEST(PrivilegedFileOperationTest, RejectsMalformedBase64)
{
    EXPECT_EQ(decodePrivilegedRequest("not valid base64!", nullptr).status,
              PrivilegeStatus::InvalidRequest);
}

TEST(PrivilegedFileOperationTest, RejectsDataAfterBase64Padding)
{
    EXPECT_EQ(decodePrivilegedRequest("YQ=A", nullptr).status,
              PrivilegeStatus::InvalidRequest);
}
