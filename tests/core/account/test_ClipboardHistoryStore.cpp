#include <gtest/gtest.h>

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QTemporaryDir>
#include <QUrl>

#ifdef Q_OS_WIN
#include <aclapi.h>
#include <sddl.h>
#include <windows.h>
#endif

#include <algorithm>
#include <string>
#include <vector>

#include "account/ClipboardHistoryStore.h"

namespace {

#ifdef Q_OS_WIN
std::string privateWindowsAclProblem(const QString &path) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return "could not open the process token";
    DWORD tokenInfoSize = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &tokenInfoSize);
    std::vector<BYTE> tokenInfo(tokenInfoSize);
    const bool hasTokenUser =
        tokenInfoSize > 0 &&
        GetTokenInformation(token, TokenUser, tokenInfo.data(), tokenInfoSize, &tokenInfoSize);
    CloseHandle(token);
    if (!hasTokenUser)
        return "could not read the token user";
    const PSID userSid = reinterpret_cast<TOKEN_USER *>(tokenInfo.data())->User.Sid;

    std::wstring widePath = path.toStdWString();
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL dacl = nullptr;
    const DWORD status = GetNamedSecurityInfoW(
        widePath.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr,
        &descriptor);
    if (status != ERROR_SUCCESS || !descriptor || !dacl) {
        if (descriptor)
            LocalFree(descriptor);
        return "GetNamedSecurityInfoW failed with " + std::to_string(status);
    }

    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    if (!GetSecurityDescriptorControl(descriptor, &control, &revision)) {
        LocalFree(descriptor);
        return "could not read the security descriptor control bits";
    }
    if (!(control & SE_DACL_PROTECTED)) {
        LocalFree(descriptor);
        return "the DACL still inherits";
    }

    ACL_SIZE_INFORMATION aclInfo{};
    if (!GetAclInformation(dacl, &aclInfo, sizeof(aclInfo), AclSizeInformation)) {
        LocalFree(descriptor);
        return "could not read the ACL contents";
    }

    BYTE systemBuffer[SECURITY_MAX_SID_SIZE];
    DWORD systemSize = sizeof(systemBuffer);
    BYTE administratorsBuffer[SECURITY_MAX_SID_SIZE];
    DWORD administratorsSize = sizeof(administratorsBuffer);
    if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, systemBuffer, &systemSize) ||
        !CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administratorsBuffer,
                            &administratorsSize)) {
        LocalFree(descriptor);
        return "could not build the well-known SIDs";
    }

    bool userHasFullAccess = false;
    std::string problem;
    for (DWORD index = 0; problem.empty() && index < aclInfo.AceCount; ++index) {
        void *rawAce = nullptr;
        if (!GetAce(dacl, index, &rawAce)) {
            problem = "could not read ACE " + std::to_string(index);
            break;
        }
        const auto *header = static_cast<const ACE_HEADER *>(rawAce);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
            problem = "ACE " + std::to_string(index) + " is not access-allowed";
            break;
        }
        const auto *ace = static_cast<const ACCESS_ALLOWED_ACE *>(rawAce);
        const PSID sid = reinterpret_cast<PSID>(const_cast<DWORD *>(&ace->SidStart));
        const bool isUser = EqualSid(sid, userSid);
        if (!isUser && !EqualSid(sid, systemBuffer) && !EqualSid(sid, administratorsBuffer)) {
            problem = "ACE " + std::to_string(index) + " grants another principal access";
            break;
        }
        if (isUser && ((ace->Mask & GENERIC_ALL) == GENERIC_ALL ||
                       (ace->Mask & FILE_ALL_ACCESS) == FILE_ALL_ACCESS)) {
            userHasFullAccess = true;
        }
    }
    LocalFree(descriptor);
    return problem.empty() && !userHasFullAccess ? "no ACE gives the user full access" : problem;
}
#endif

void expectPrivatePath(const QString &path, bool directory) {
#ifdef Q_OS_WIN
    EXPECT_EQ(privateWindowsAclProblem(path), std::string()) << "on " << path.toStdString();
#else
    const QFileDevice::Permissions privateDirectory =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
        QFileDevice::ReadUser | QFileDevice::WriteUser | QFileDevice::ExeUser;
    const QFileDevice::Permissions privateFile =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner |
        QFileDevice::ReadUser | QFileDevice::WriteUser;
    EXPECT_EQ(QFileInfo(path).permissions(), directory ? privateDirectory : privateFile);
#endif
}

TEST(ClipboardHistoryStoreTest, ImageWinsOverHtmlAndTextFallback) {
    QMimeData mime;
    mime.setImageData(QImage(8, 8, QImage::Format_ARGB32));
    mime.setHtml(QStringLiteral("<b>fallback</b>"));
    mime.setText(QStringLiteral("fallback"));

    ClipboardCapture capture;
    ASSERT_TRUE(ClipboardHistoryStore::captureFromMimeData(&mime, &capture));
    EXPECT_EQ(capture.kind, ClipboardRecordKind::Image);
}

TEST(ClipboardHistoryStoreTest, RichTextStoresPlainTextOnly) {
    QMimeData mime;
    mime.setHtml(QStringLiteral("<b>Hello</b>"));
    mime.setText(QStringLiteral("Hello"));

    ClipboardCapture capture;
    ASSERT_TRUE(ClipboardHistoryStore::captureFromMimeData(&mime, &capture));
    EXPECT_EQ(capture.kind, ClipboardRecordKind::Text);
    EXPECT_EQ(capture.text, QStringLiteral("Hello"));
}

TEST(ClipboardHistoryStoreTest, RejectsFilesAndPrivateFormats) {
    QMimeData files;
    files.setUrls({QUrl::fromLocalFile(QStringLiteral("C:/secret.txt"))});
    EXPECT_FALSE(ClipboardHistoryStore::captureFromMimeData(&files, nullptr));

    QMimeData privateMime;
    privateMime.setData("application/x-password", "secret");
    privateMime.setText(QStringLiteral("secret"));
    EXPECT_FALSE(ClipboardHistoryStore::captureFromMimeData(&privateMime, nullptr));
}

QByteArray encodedPng(const QImage &image) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (buffer.open(QIODevice::WriteOnly))
        image.save(&buffer, "PNG");
    return bytes;
}

QString historyPath(const QTemporaryDir &dir) {
    return dir.filePath(QStringLiteral("cloud-clipboard/history"));
}

TEST(ClipboardHistoryStoreTest, PersistsFiftyNewestAndDeduplicates) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ClipboardHistoryStore store(dir.path());
    for (int i = 0; i < 51; ++i)
        ASSERT_FALSE(store.addLocalText(QString::number(i)).id.isEmpty());
    ASSERT_FALSE(store.addLocalText(QStringLiteral("50")).id.isEmpty());

    ClipboardHistoryStore reloaded(dir.path());
    ASSERT_TRUE(reloaded.load());
    ASSERT_EQ(reloaded.records().size(), 50);
    EXPECT_EQ(reloaded.records().first().text, QStringLiteral("50"));
    EXPECT_EQ(std::count_if(reloaded.records().cbegin(), reloaded.records().cend(),
                            [](const ClipboardHistoryRecord &record) {
                                return record.text == QLatin1String("50");
                            }),
              1);
    EXPECT_TRUE(reloaded.records().back().text != QLatin1String("0"));
}

TEST(ClipboardHistoryStoreTest, ReencodesImagesAndPersistsTheirPrivateRelativeReference) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QImage image(8, 6, QImage::Format_ARGB32);
    image.fill(Qt::red);

    ClipboardHistoryStore store(dir.path());
    const ClipboardHistoryRecord added = store.addLocalImage(encodedPng(image),
                                                              QStringLiteral("image/jpeg"), 8, 6);
    ASSERT_FALSE(added.id.isEmpty());
    EXPECT_EQ(added.kind, ClipboardRecordKind::Image);
    EXPECT_EQ(added.mime, QStringLiteral("image/png"));
    EXPECT_TRUE(QFileInfo::exists(added.imagePath));

    QFile imageFile(added.imagePath);
    ASSERT_TRUE(imageFile.open(QIODevice::ReadOnly));
    const QByteArray stored = imageFile.readAll();
    EXPECT_EQ(added.size, stored.size());
    EXPECT_EQ(added.sha256, QString::fromLatin1(
                                QCryptographicHash::hash(stored, QCryptographicHash::Sha256).toHex()));
    QImage decoded;
    ASSERT_TRUE(decoded.loadFromData(stored));
    EXPECT_EQ(decoded.size(), image.size());

    ClipboardHistoryStore reloaded(dir.path());
    ASSERT_TRUE(reloaded.load());
    ASSERT_EQ(reloaded.records().size(), 1);
    EXPECT_EQ(reloaded.records().first().id, added.id);
    EXPECT_EQ(reloaded.records().first().imagePath, added.imagePath);

    QFile manifest(QDir(historyPath(dir)).filePath(QStringLiteral("manifest.json")));
    ASSERT_TRUE(manifest.open(QIODevice::ReadOnly));
    EXPECT_FALSE(manifest.readAll().contains(dir.path().toUtf8()));
    manifest.close();
    expectPrivatePath(historyPath(dir), true);
    expectPrivatePath(QDir(historyPath(dir)).filePath(QStringLiteral("images")), true);
    expectPrivatePath(manifest.fileName(), false);
    expectPrivatePath(added.imagePath, false);
}

TEST(ClipboardHistoryStoreTest, QuarantinesCorruptManifestAndRebuildsEmptyHistory) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir().mkpath(historyPath(dir)));
    QFile manifest(QDir(historyPath(dir)).filePath(QStringLiteral("manifest.json")));
    ASSERT_TRUE(manifest.open(QIODevice::WriteOnly));
    ASSERT_EQ(manifest.write("not json"), 8);
    manifest.close();

    ClipboardHistoryStore store(dir.path());
    ASSERT_TRUE(store.load());
    EXPECT_TRUE(store.records().isEmpty());
    EXPECT_TRUE(QFileInfo::exists(manifest.fileName()));
    EXPECT_TRUE(QFileInfo::exists(QDir(historyPath(dir)).filePath(
        QStringLiteral("manifest.json.corrupt"))));
}

TEST(ClipboardHistoryStoreTest, RemovesOrphanedImageFilesDuringLoad) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString images = QDir(historyPath(dir)).filePath(QStringLiteral("images"));
    ASSERT_TRUE(QDir().mkpath(images));
    const QString orphan = QDir(images).filePath(QStringLiteral("orphan.bin"));
    QFile file(orphan);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write("orphan"), 6);
    file.close();

    ClipboardHistoryStore store(dir.path());
    ASSERT_TRUE(store.load());
    EXPECT_FALSE(QFileInfo::exists(orphan));
}

TEST(ClipboardHistoryStoreTest, RemovesRecordsAndTheirImageFiles) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QImage image(2, 2, QImage::Format_ARGB32);
    image.fill(Qt::blue);
    ClipboardHistoryStore store(dir.path());
    const ClipboardHistoryRecord added = store.addLocalImage(encodedPng(image),
                                                              QStringLiteral("image/png"), 2, 2);
    ASSERT_FALSE(added.id.isEmpty());

    ClipboardHistoryRecord found;
    ASSERT_TRUE(store.lookup(added.id, &found));
    EXPECT_EQ(found.imagePath, added.imagePath);
    ASSERT_TRUE(store.remove(added.id));
    EXPECT_FALSE(store.lookup(added.id, nullptr));
    EXPECT_FALSE(QFileInfo::exists(added.imagePath));
}

TEST(ClipboardHistoryStoreTest, PreservesIncomingProvenanceAfterPersistentStorage) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QDateTime deliveredAt =
        QDateTime::fromString(QStringLiteral("2026-08-23T12:00:00.123Z"), Qt::ISODateWithMs);
    ClipboardHistoryStore store(dir.path());
    const ClipboardHistoryRecord added = store.addIncomingText(
        QStringLiteral("from another device"), QStringLiteral("device-2"), QStringLiteral("Laptop"),
        deliveredAt);
    ASSERT_FALSE(added.id.isEmpty());
    EXPECT_EQ(added.origin, ClipboardRecordOrigin::Incoming);
    EXPECT_EQ(added.sourceDeviceId, QStringLiteral("device-2"));
    EXPECT_EQ(added.sourceDeviceName, QStringLiteral("Laptop"));
    EXPECT_EQ(added.created, deliveredAt);

    ClipboardHistoryStore reloaded(dir.path());
    ASSERT_TRUE(reloaded.load());
    ASSERT_EQ(reloaded.records().size(), 1);
    EXPECT_EQ(reloaded.records().first().origin, ClipboardRecordOrigin::Incoming);
    EXPECT_EQ(reloaded.records().first().sourceDeviceId, QStringLiteral("device-2"));
    EXPECT_EQ(reloaded.records().first().created, deliveredAt);
}

TEST(ClipboardHistoryStoreTest, LoadsLegacyV1TimestampWithoutDeletingItsImage) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString history = historyPath(dir);
    const QString images = QDir(history).filePath(QStringLiteral("images"));
    ASSERT_TRUE(QDir().mkpath(images));

    QImage image(3, 2, QImage::Format_ARGB32);
    image.fill(Qt::green);
    const QByteArray imageBytes = encodedPng(image);
    const QString id = QStringLiteral("3b20b7ee-aeca-4f1e-a950-401880dfd9f7");
    const QString imagePath = QDir(images).filePath(id + QStringLiteral(".bin"));
    QFile imageFile(imagePath);
    ASSERT_TRUE(imageFile.open(QIODevice::WriteOnly));
    ASSERT_EQ(imageFile.write(imageBytes), imageBytes.size());
    imageFile.close();

    QJsonObject record;
    record.insert(QStringLiteral("id"), id);
    record.insert(QStringLiteral("origin"), QStringLiteral("local"));
    record.insert(QStringLiteral("kind"), QStringLiteral("image"));
    record.insert(QStringLiteral("text"), QString());
    record.insert(QStringLiteral("mime"), QStringLiteral("image/png"));
    record.insert(QStringLiteral("sha256"), QString::fromLatin1(
        QCryptographicHash::hash(imageBytes, QCryptographicHash::Sha256).toHex()));
    record.insert(QStringLiteral("sourceDeviceId"), QString());
    record.insert(QStringLiteral("sourceDeviceName"), QString());
    record.insert(QStringLiteral("size"), static_cast<double>(imageBytes.size()));
    record.insert(QStringLiteral("width"), image.width());
    record.insert(QStringLiteral("height"), image.height());
    record.insert(QStringLiteral("created"), QStringLiteral("2026-08-23T12:00:00Z"));
    QJsonObject manifest;
    manifest.insert(QStringLiteral("version"), 1);
    manifest.insert(QStringLiteral("records"), QJsonArray{record});
    QFile manifestFile(QDir(history).filePath(QStringLiteral("manifest.json")));
    ASSERT_TRUE(manifestFile.open(QIODevice::WriteOnly));
    ASSERT_EQ(manifestFile.write(QJsonDocument(manifest).toJson(QJsonDocument::Compact)),
              QJsonDocument(manifest).toJson(QJsonDocument::Compact).size());
    manifestFile.close();

    ClipboardHistoryStore store(dir.path());
    ASSERT_TRUE(store.load());
    ASSERT_EQ(store.records().size(), 1);
    EXPECT_TRUE(store.records().first().created.isValid());
    EXPECT_EQ(store.records().first().id, id);
    EXPECT_TRUE(QFileInfo::exists(imagePath));
}

} // namespace
