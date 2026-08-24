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
#include <filesystem>
#include <string>
#include <system_error>
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

TEST(ClipboardHistoryStoreTest, HtmlOnlyClipboardIsConvertedToPlainText) {
    QMimeData mime;
    mime.setHtml(QStringLiteral("<p>Hello &amp; goodbye<br>next line</p>"));

    ClipboardCapture capture;
    ASSERT_TRUE(ClipboardHistoryStore::captureFromMimeData(&mime, &capture));
    EXPECT_EQ(capture.kind, ClipboardRecordKind::Text);
    EXPECT_EQ(capture.text, QStringLiteral("Hello & goodbye\nnext line"));
    EXPECT_FALSE(capture.text.contains(QLatin1Char('<')));
    EXPECT_FALSE(capture.text.contains(QLatin1Char('>')));
}

TEST(ClipboardHistoryStoreTest, ExplicitPlainTextFallbackWinsOverHtml) {
    QMimeData mime;
    mime.setHtml(QStringLiteral("<p>formatted source</p>"));
    const QString plain = QStringLiteral("if (left < right && right > 0) return \"&amp;\";");
    mime.setText(plain);

    ClipboardCapture capture;
    ASSERT_TRUE(ClipboardHistoryStore::captureFromMimeData(&mime, &capture));
    EXPECT_EQ(capture.kind, ClipboardRecordKind::Text);
    EXPECT_EQ(capture.text, plain);
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

TEST(ClipboardHistoryStoreTest, CapturesSingleLocalImageFileFromClipboardUrls) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString imagePath = directory.filePath(QStringLiteral("copied-image.png"));
    QImage source(32, 24, QImage::Format_ARGB32);
    source.fill(Qt::magenta);
    ASSERT_TRUE(source.save(imagePath, "PNG"));

    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(imagePath)});
    ClipboardCapture capture;
    ASSERT_TRUE(ClipboardHistoryStore::captureFromMimeData(&mime, &capture));
    EXPECT_EQ(capture.kind, ClipboardRecordKind::Image);
    EXPECT_EQ(capture.image.size(), source.size());

    QMimeData multiple;
    multiple.setUrls({QUrl::fromLocalFile(imagePath), QUrl::fromLocalFile(imagePath)});
    EXPECT_FALSE(ClipboardHistoryStore::captureFromMimeData(&multiple));
}

TEST(ClipboardHistoryStoreTest, MatchingLocalAndIncomingContentKeepSeparateOrigins) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ClipboardHistoryStore store(directory.path());
    const ClipboardHistoryRecord incoming = store.addIncomingText(QStringLiteral("same bytes"),
                                                                   QStringLiteral("device-a"), QStringLiteral("A"));
    const ClipboardHistoryRecord local = store.addLocalText(QStringLiteral("same bytes"));
    const ClipboardHistoryRecord secondIncoming = store.addIncomingText(QStringLiteral("same bytes"),
                                                                         QStringLiteral("device-b"), QStringLiteral("B"));
    ASSERT_FALSE(incoming.id.isEmpty());
    ASSERT_FALSE(local.id.isEmpty());
    ASSERT_FALSE(secondIncoming.id.isEmpty());
    EXPECT_NE(incoming.id, local.id);
    EXPECT_NE(incoming.id, secondIncoming.id);
    EXPECT_EQ(store.records().size(), 3);
    EXPECT_EQ(store.records().at(1).origin, ClipboardRecordOrigin::Local);
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

bool createDirectorySymlink(const QString &target, const QString &link) {
    std::error_code error;
    std::filesystem::create_directory_symlink(std::filesystem::path(target.toStdWString()),
                                              std::filesystem::path(link.toStdWString()), error);
    return !error;
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

TEST(ClipboardHistoryStoreTest, RemovesMultipleRecordsAtomicallyAndPreservesSurvivors) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ClipboardHistoryStore store(dir.path());
    const ClipboardHistoryRecord oldest = store.addLocalText(QStringLiteral("oldest"));
    const ClipboardHistoryRecord incoming = store.addIncomingText(
        QStringLiteral("incoming"), QStringLiteral("device-2"), QStringLiteral("Laptop"));
    QImage image(3, 2, QImage::Format_ARGB32);
    image.fill(Qt::yellow);
    const ClipboardHistoryRecord imageRecord = store.addLocalImage(
        encodedPng(image), QStringLiteral("image/png"), image.width(), image.height());
    const ClipboardHistoryRecord newest = store.addLocalText(QStringLiteral("newest"));
    ASSERT_FALSE(oldest.id.isEmpty());
    ASSERT_FALSE(incoming.id.isEmpty());
    ASSERT_FALSE(imageRecord.id.isEmpty());
    ASSERT_FALSE(newest.id.isEmpty());
    ASSERT_TRUE(QFileInfo::exists(imageRecord.imagePath));

    ASSERT_TRUE(store.removeRecords({imageRecord.id, incoming.id, imageRecord.id, QString(),
                                     QStringLiteral("missing")}));
    ASSERT_EQ(store.records().size(), 2);
    EXPECT_EQ(store.records().at(0).id, newest.id);
    EXPECT_EQ(store.records().at(1).id, oldest.id);
    EXPECT_FALSE(QFileInfo::exists(imageRecord.imagePath));
    EXPECT_FALSE(store.lookup(imageRecord.id));
    EXPECT_FALSE(store.lookup(incoming.id));

    ClipboardHistoryStore reloaded(dir.path());
    ASSERT_TRUE(reloaded.load());
    ASSERT_EQ(reloaded.records().size(), 2);
    EXPECT_EQ(reloaded.records().at(0).id, newest.id);
    EXPECT_EQ(reloaded.records().at(1).id, oldest.id);
}

TEST(ClipboardHistoryStoreTest, RejectsBatchRemovalWhenNoKnownRecordIsRequested) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ClipboardHistoryStore store(dir.path());
    const ClipboardHistoryRecord kept = store.addLocalText(QStringLiteral("keep"));
    ASSERT_FALSE(kept.id.isEmpty());

    EXPECT_FALSE(store.removeRecords({QString(), QStringLiteral("missing"), QStringLiteral("missing")}));
    ASSERT_EQ(store.records().size(), 1);
    EXPECT_EQ(store.records().first().id, kept.id);

    ClipboardHistoryStore reloaded(dir.path());
    ASSERT_TRUE(reloaded.load());
    ASSERT_EQ(reloaded.records().size(), 1);
    EXPECT_EQ(reloaded.records().first().id, kept.id);
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

TEST(ClipboardHistoryStoreTest, RemovesRetiredLegacyImagesWithoutTouchingHistoryOrSiblingFiles) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString clipboardRoot = dir.filePath(QStringLiteral("cloud-clipboard"));
    const QString legacyImages = QDir(clipboardRoot).filePath(QStringLiteral("images"));
    ASSERT_TRUE(QDir().mkpath(legacyImages));

    const QString legacyImage = QDir(legacyImages).filePath(
        QStringLiteral("0123456789abcdef0123456789abcdef.bin"));
    QFile legacyFile(legacyImage);
    ASSERT_TRUE(legacyFile.open(QIODevice::WriteOnly));
    ASSERT_EQ(legacyFile.write("old-image"), 9);
    legacyFile.close();

    const QString sibling = QDir(clipboardRoot).filePath(QStringLiteral("keep.txt"));
    QFile siblingFile(sibling);
    ASSERT_TRUE(siblingFile.open(QIODevice::WriteOnly));
    ASSERT_EQ(siblingFile.write("keep"), 4);
    siblingFile.close();

    ClipboardHistoryStore store(dir.path());
    ASSERT_TRUE(store.load());
    EXPECT_FALSE(QFileInfo::exists(legacyImages));
    EXPECT_TRUE(QFileInfo::exists(sibling));
    EXPECT_TRUE(QFileInfo::exists(QDir(historyPath(dir)).filePath(QStringLiteral("manifest.json"))));

    // The migration is safe to repeat if the app is restarted after a partial upgrade.
    EXPECT_TRUE(store.load());
    EXPECT_FALSE(QFileInfo::exists(legacyImages));
    EXPECT_TRUE(QFileInfo::exists(sibling));
}

TEST(ClipboardHistoryStoreTest, LeavesUnexpectedRetiredCacheEntriesUntouched) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString legacyImages = dir.filePath(QStringLiteral("cloud-clipboard/images"));
    ASSERT_TRUE(QDir().mkpath(legacyImages));

    const QString recognized = QDir(legacyImages).filePath(
        QStringLiteral("0123456789abcdef0123456789abcdef.bin"));
    QFile recognizedFile(recognized);
    ASSERT_TRUE(recognizedFile.open(QIODevice::WriteOnly));
    ASSERT_EQ(recognizedFile.write("retired"), 7);
    recognizedFile.close();

    const QString unexpectedFile = QDir(legacyImages).filePath(QStringLiteral("keep.txt"));
    QFile keepFile(unexpectedFile);
    ASSERT_TRUE(keepFile.open(QIODevice::WriteOnly));
    ASSERT_EQ(keepFile.write("keep"), 4);
    keepFile.close();
    const QString unexpectedDirectory = QDir(legacyImages).filePath(QStringLiteral("nested"));
    ASSERT_TRUE(QDir().mkpath(unexpectedDirectory));
    const QString nestedFile = QDir(unexpectedDirectory).filePath(QStringLiteral("keep.bin"));
    QFile nestedKeepFile(nestedFile);
    ASSERT_TRUE(nestedKeepFile.open(QIODevice::WriteOnly));
    ASSERT_EQ(nestedKeepFile.write("keep"), 4);
    nestedKeepFile.close();

    ClipboardHistoryStore store(dir.path());
    ASSERT_TRUE(store.load());
    EXPECT_FALSE(QFileInfo::exists(recognized));
    EXPECT_TRUE(QFileInfo::exists(unexpectedFile));
    EXPECT_TRUE(QFileInfo::exists(nestedFile));
    EXPECT_TRUE(QFileInfo::exists(legacyImages));
}

TEST(ClipboardHistoryStoreTest, RetiredLegacyImageCleanupNeverFollowsNestedSymlinks) {
    QTemporaryDir dir;
    QTemporaryDir outside;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(outside.isValid());
    const QString legacyImages = dir.filePath(QStringLiteral("cloud-clipboard/images"));
    ASSERT_TRUE(QDir().mkpath(legacyImages));

    const QString outsideFile = outside.filePath(QStringLiteral("must-survive.bin"));
    QFile protectedFile(outsideFile);
    ASSERT_TRUE(protectedFile.open(QIODevice::WriteOnly));
    ASSERT_EQ(protectedFile.write("protected"), 9);
    protectedFile.close();

    const QString nestedLink = QDir(legacyImages).filePath(QStringLiteral("outside-link"));
    if (!createDirectorySymlink(outside.path(), nestedLink))
        GTEST_SKIP() << "Directory symlinks are unavailable in this test environment";

    ClipboardHistoryStore store(dir.path());
    ASSERT_TRUE(store.load());
    EXPECT_FALSE(QFileInfo::exists(legacyImages));
    EXPECT_TRUE(QFileInfo::exists(outsideFile));
}

TEST(ClipboardHistoryStoreTest, RetiredLegacyImageCleanupRemovesOnlyRootSymlink) {
    QTemporaryDir dir;
    QTemporaryDir outside;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(outside.isValid());
    const QString legacyParent = dir.filePath(QStringLiteral("cloud-clipboard"));
    ASSERT_TRUE(QDir().mkpath(legacyParent));

    const QString outsideFile = outside.filePath(QStringLiteral("must-survive.bin"));
    QFile protectedFile(outsideFile);
    ASSERT_TRUE(protectedFile.open(QIODevice::WriteOnly));
    ASSERT_EQ(protectedFile.write("protected"), 9);
    protectedFile.close();

    const QString legacyImages = QDir(legacyParent).filePath(QStringLiteral("images"));
    if (!createDirectorySymlink(outside.path(), legacyImages))
        GTEST_SKIP() << "Directory symlinks are unavailable in this test environment";

    ClipboardHistoryStore store(dir.path());
    ASSERT_TRUE(store.load());
    EXPECT_FALSE(QFileInfo(legacyImages).isSymbolicLink());
    EXPECT_FALSE(QFileInfo::exists(legacyImages));
    EXPECT_TRUE(QFileInfo::exists(outsideFile));
    EXPECT_TRUE(QFileInfo::exists(QDir(historyPath(dir)).filePath(QStringLiteral("manifest.json"))));
}

} // namespace
