#include <gtest/gtest.h>

#include "TryUntil.h"

#include <QCheckBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QTemporaryDir>
#include <QTest>
#include <QVector>

#include <atomic>
#include <memory>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "FileInfo.h"
#include "dialogs/DirectoryStatisticsTask.h"
#include "dialogs/PropertiesDialog.h"
#include "TranslationManager.h"

namespace {

// A listing entry as a network backend hands one over: every field is
// pre-fetched, and `path` is the SERVER's path -- nothing on this machine.
FileInfo remoteFile(const QString &path, qint64 size, QFile::Permissions perms) {
    return FileInfo::fromFields(path, QFileInfo(path).fileName(), size,
                                QDateTime::currentDateTime(), false, perms);
}

FileInfo remoteDir(const QString &path) {
    return FileInfo::fromFields(path, QFileInfo(path).fileName(), 4096,
                                QDateTime::currentDateTime(), true, QFile::ReadOwner);
}

// Whether any value label in the dialog contains `needle` -- how the size and
// count rows are checked without reaching into the layout.
bool showsText(const PropertiesDialog &dlg, const QString &needle) {
    for (const QLabel *label : dlg.findChildren<const QLabel *>())
        if (label->text().contains(needle))
            return true;
    return false;
}

QLabel *valueLabel(PropertiesDialog &dlg, const char *objectName) {
    return dlg.findChild<QLabel *>(QString::fromLatin1(objectName));
}

bool createDirectorySymlink(const QString &target, const QString &linkPath) {
#ifdef Q_OS_WIN
    const DWORD flags = SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2; // allow unprivileged creation
    return CreateSymbolicLinkW(reinterpret_cast<LPCWSTR>(linkPath.utf16()),
                               reinterpret_cast<LPCWSTR>(target.utf16()), flags) != FALSE;
#else
    return QFile::link(target, linkPath);
#endif
}

} // namespace

TEST(DirectoryStatisticsTaskTest, RecursivelyCountsFilesAndBytesWithoutFollowingDirectoryLinks) {
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    QDir root(temp.path());
    ASSERT_TRUE(root.mkpath(QStringLiteral("root/nested")));
    ASSERT_TRUE(root.mkpath(QStringLiteral("outside")));

    const auto writeFile = [](const QString &path, const QByteArray &contents) {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            return false;
        return file.write(contents) == contents.size();
    };
    ASSERT_TRUE(writeFile(root.filePath(QStringLiteral("root/a.bin")), QByteArray("a")));
    ASSERT_TRUE(writeFile(root.filePath(QStringLiteral("root/nested/b.bin")), QByteArray("bc")));
    ASSERT_TRUE(writeFile(root.filePath(QStringLiteral("outside/ignored.bin")), QByteArray("ignored")));

    const QString linkPath = root.filePath(QStringLiteral("root/outside-link"));
    if (!createDirectorySymlink(root.filePath(QStringLiteral("outside")), linkPath))
        GTEST_SKIP() << "directory symlinks are unavailable for this test account";

    auto cancel = std::make_shared<std::atomic_bool>(false);
    const DirectoryStatisticsTask::Result result =
        DirectoryStatisticsTask::scanPaths({root.filePath(QStringLiteral("root"))}, cancel);
    EXPECT_FALSE(result.cancelled);
    EXPECT_EQ(result.fileCount, 2);
    EXPECT_EQ(result.bytes, 3);
}

TEST(DirectoryStatisticsTaskTest, HonoursCancellationBeforeWalking) {
    auto cancel = std::make_shared<std::atomic_bool>(true);
    const DirectoryStatisticsTask::Result result =
        DirectoryStatisticsTask::scanPaths({QStringLiteral("unused")}, cancel);
    EXPECT_TRUE(result.cancelled);
    EXPECT_EQ(result.fileCount, 0);
    EXPECT_EQ(result.bytes, 0);
}

// The octal <-> QFile::Permissions conversion is pure flag math and needs no
// QApplication, so it is safe to exercise here.

TEST(PropertiesDialogTest, ToOctalMapsStandardModes) {
    QFile::Permissions rwxrxrx = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
                                  QFile::ReadGroup | QFile::ExeGroup | QFile::ReadOther |
                                  QFile::ExeOther;
    EXPECT_EQ(PropertiesDialog::toOctal(rwxrxrx), 0755);

    QFile::Permissions rw_r__r__ =
        QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther;
    EXPECT_EQ(PropertiesDialog::toOctal(rw_r__r__), 0644);

    EXPECT_EQ(PropertiesDialog::toOctal(QFile::Permissions()), 0);
}

TEST(PropertiesDialogTest, FromOctalIsInverseOfToOctal) {
    for (int mode : {0000, 0400, 0644, 0700, 0755, 0777, 0111, 0666}) {
        EXPECT_EQ(PropertiesDialog::toOctal(PropertiesDialog::fromOctal(mode)), mode)
            << "mode " << mode;
    }
}

TEST(PropertiesDialogTest, FromOctalSetsExpectedFlags) {
    QFile::Permissions p = PropertiesDialog::fromOctal(0640);
    EXPECT_TRUE(p & QFile::ReadOwner);
    EXPECT_TRUE(p & QFile::WriteOwner);
    EXPECT_FALSE(p & QFile::ExeOwner);
    EXPECT_TRUE(p & QFile::ReadGroup);
    EXPECT_FALSE(p & QFile::WriteGroup);
    EXPECT_FALSE(p & QFile::ReadOther);
}

// --- Multi-selection summary off the cached listing ------------------------
//
// The bug: selecting three files on a network tab reported "Total size: 0 B",
// because the summary stat-ed the server's paths locally. The sizes were in the
// listing the panel already had.

TEST(PropertiesDialogTest, TotalFileSizeSumsFilesAndIgnoresDirectories) {
    const QVector<FileInfo> infos = {
        remoteFile("/share/a.pdf", 3 * 1024 * 1024, PropertiesDialog::fromOctal(0644)),
        remoteDir("/share/sub"),
        remoteFile("/share/b.iso", 5 * 1024 * 1024, PropertiesDialog::fromOctal(0644)),
    };
    EXPECT_EQ(PropertiesDialog::totalFileSize(infos), 8LL * 1024 * 1024);
}

TEST(PropertiesDialogTest, TotalFileSizeOfNothingIsZero) {
    EXPECT_EQ(PropertiesDialog::totalFileSize({}), 0);
}

TEST(PropertiesDialogTest, RemoteMultiSelectionShowsTheListingsTotalSize) {
    const QVector<FileInfo> infos = {
        remoteFile("/share/a.pdf", 3 * 1024 * 1024, PropertiesDialog::fromOctal(0644)),
        remoteFile("/share/b.iso", 5 * 1024 * 1024, PropertiesDialog::fromOctal(0644)),
        remoteFile("/share/c.txt", 0, PropertiesDialog::fromOctal(0644)),
    };
    PropertiesDialog dlg(infos);
    EXPECT_TRUE(showsText(dlg, QStringLiteral("3 items")));
    EXPECT_TRUE(showsText(dlg, QStringLiteral("8.0 MB")))
        << "a multi-selection on a network tab must be sized from the listing, not from a "
           "local stat of the server's paths";
}

TEST(PropertiesDialogTest, UsesAFixedTitleForSingleAndMultiSelection) {
    PropertiesDialog single(QStringLiteral("C:/long-name.txt"));
    PropertiesDialog multiple(
        QStringList{QStringLiteral("C:/one.txt"), QStringLiteral("C:/two.txt")});

    EXPECT_EQ(single.windowTitle(), QStringLiteral("Properties"));
    EXPECT_EQ(multiple.windowTitle(), QStringLiteral("Properties"));
}

TEST(PropertiesDialogTest, LongNameValueWrapsWithoutGrowingPastTheScreen) {
    const QString longName(4096, QLatin1Char('x'));
    PropertiesDialog dlg(QStringLiteral("C:/") + longName);
    QLabel *name = valueLabel(dlg, "propertiesNameValue");
    ASSERT_NE(name, nullptr);

    EXPECT_TRUE(name->wordWrap());
    EXPECT_TRUE(name->textInteractionFlags() & Qt::TextSelectableByMouse);
    EXPECT_LE(dlg.maximumWidth(), QGuiApplication::primaryScreen()->availableGeometry().width());

    dlg.show();
    QTest::qWait(1);
    EXPECT_LE(dlg.width(), QGuiApplication::primaryScreen()->availableGeometry().width());
}

#ifdef Q_OS_WIN
TEST(PropertiesDialogTest, WindowsAllPropertiesButtonOnlyEnablesForOneLocalItem) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("local.txt"));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.close();

    PropertiesDialog single(path);
    PropertiesDialog multiple(QStringList{path, path});
    PropertiesDialog remote(remoteFile(QStringLiteral("/share/local.txt"), 1,
                                       PropertiesDialog::fromOctal(0644)));

    auto *singleButton = single.findChild<QPushButton *>(QStringLiteral("propertiesAllButton"));
    auto *multipleButton =
        multiple.findChild<QPushButton *>(QStringLiteral("propertiesAllButton"));
    auto *remoteButton = remote.findChild<QPushButton *>(QStringLiteral("propertiesAllButton"));
    ASSERT_NE(singleButton, nullptr);
    ASSERT_NE(multipleButton, nullptr);
    ASSERT_NE(remoteButton, nullptr);
    EXPECT_EQ(singleButton->text(), QStringLiteral("All Properties"));
    EXPECT_TRUE(singleButton->isEnabled());
    EXPECT_FALSE(multipleButton->isEnabled());
    EXPECT_FALSE(remoteButton->isEnabled());

    single.show();
    QTest::qWait(1);
    auto *buttonBox = single.findChild<QDialogButtonBox *>();
    ASSERT_NE(buttonBox, nullptr);
    EXPECT_LT(singleButton->mapTo(&single, QPoint()).x(),
              buttonBox->mapTo(&single, QPoint()).x());
}
#else
TEST(PropertiesDialogTest, LinuxDoesNotShowTheWindowsAllPropertiesButton) {
    PropertiesDialog dlg(QStringLiteral("/tmp/local.txt"));
    EXPECT_EQ(dlg.findChild<QPushButton *>(QStringLiteral("propertiesAllButton")), nullptr);
}
#endif

TEST(PropertiesDialogTest, RemoteDirectoryMarksRecursiveStatisticsUnavailable) {
    PropertiesDialog dlg(remoteDir(QStringLiteral("/share/archive")));
    QLabel *size = valueLabel(dlg, "propertiesSizeValue");
    QLabel *contains = valueLabel(dlg, "propertiesContainsValue");
    ASSERT_NE(size, nullptr);
    ASSERT_NE(contains, nullptr);
    EXPECT_EQ(size->text(), QStringLiteral("Unavailable"));
    EXPECT_EQ(contains->text(), QStringLiteral("Unavailable"));
}

TEST(PropertiesDialogTest, LocalDirectoryStatisticsStartAsCalculatingThenCompleteAsynchronously) {
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    QDir root(temp.path());
    ASSERT_TRUE(root.mkpath(QStringLiteral("folder/nested")));

    QFile first(root.filePath(QStringLiteral("folder/first.bin")));
    ASSERT_TRUE(first.open(QIODevice::WriteOnly));
    ASSERT_EQ(first.write("a"), 1);
    first.close();

    QFile second(root.filePath(QStringLiteral("folder/nested/second.bin")));
    ASSERT_TRUE(second.open(QIODevice::WriteOnly));
    ASSERT_EQ(second.write("bc"), 2);
    second.close();

    PropertiesDialog dlg(root.filePath(QStringLiteral("folder")));
    QLabel *size = valueLabel(dlg, "propertiesSizeValue");
    QLabel *contains = valueLabel(dlg, "propertiesContainsValue");
    ASSERT_NE(size, nullptr);
    ASSERT_NE(contains, nullptr);
    EXPECT_EQ(size->text(), QStringLiteral("Calculating..."));
    EXPECT_EQ(contains->text(), QStringLiteral("Calculating..."));

    FC_TRY_COMPARE_WITH_TIMEOUT(size->text(), QStringLiteral("3 B"), 5000);
    EXPECT_EQ(contains->text(), QStringLiteral("2 files"));
}

TEST(PropertiesDialogTest, ClosingDialogIgnoresLateDirectoryStatistics) {
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString folder = QDir(temp.path()).filePath(QStringLiteral("folder"));
    ASSERT_TRUE(QDir().mkpath(folder));
    QFile file(QDir(folder).filePath(QStringLiteral("item.bin")));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write("data"), 4);
    file.close();

    PropertiesDialog dlg(folder);
    QLabel *size = valueLabel(dlg, "propertiesSizeValue");
    ASSERT_NE(size, nullptr);
    EXPECT_EQ(size->text(), QStringLiteral("Calculating..."));

    dlg.reject();
    QTest::qWait(100);
    EXPECT_EQ(size->text(), QStringLiteral("Calculating..."));
}

TEST(PropertiesDialogTest, LocalFileShowsImmediateSizeAndPlatformMetadata) {
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString path = QDir(temp.path()).filePath(QStringLiteral("local.txt"));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write("hello"), 5);
    file.close();

    PropertiesDialog dlg(path);
    QLabel *size = valueLabel(dlg, "propertiesSizeValue");
    ASSERT_NE(size, nullptr);
    EXPECT_EQ(size->text(), QStringLiteral("5 B"));

#ifdef Q_OS_WIN
    EXPECT_EQ(dlg.findChild<QGroupBox *>(QStringLiteral("propertiesPermissionsGroup")), nullptr);
    EXPECT_EQ(valueLabel(dlg, "propertiesOwnerValue"), nullptr);
    EXPECT_EQ(valueLabel(dlg, "propertiesGroupValue"), nullptr);
    EXPECT_NE(valueLabel(dlg, "propertiesCreatedValue"), nullptr);
    EXPECT_NE(valueLabel(dlg, "propertiesModifiedValue"), nullptr);
    EXPECT_NE(valueLabel(dlg, "propertiesAccessedValue"), nullptr);
    EXPECT_NE(valueLabel(dlg, "propertiesAttributesValue"), nullptr);
#else
    EXPECT_NE(dlg.findChild<QGroupBox *>(QStringLiteral("propertiesPermissionsGroup")), nullptr);
    EXPECT_NE(valueLabel(dlg, "propertiesOwnerValue"), nullptr);
    EXPECT_NE(valueLabel(dlg, "propertiesGroupValue"), nullptr);
#endif
}

TEST(PropertiesDialogTest, RemovableDeviceShowsNameAndStorageMetrics) {
    RemovableDevice device;
    device.name = QStringLiteral("Backup USB");
    device.mountPoint = QStringLiteral("E:\\");
    device.devNode = QStringLiteral("\\\\.\\E:");
    device.bytesTotal = 64LL * 1024 * 1024 * 1024;
    device.bytesAvailable = 12LL * 1024 * 1024 * 1024;

    PropertiesDialog dlg(device);

    ASSERT_NE(valueLabel(dlg, "propertiesNameValue"), nullptr);
    ASSERT_NE(valueLabel(dlg, "propertiesCapacityValue"), nullptr);
    ASSERT_NE(valueLabel(dlg, "propertiesAvailableValue"), nullptr);
    EXPECT_EQ(valueLabel(dlg, "propertiesNameValue")->text(), QStringLiteral("Backup USB"));
    EXPECT_TRUE(valueLabel(dlg, "propertiesCapacityValue")->text().contains("64.0 GB"));
    EXPECT_TRUE(valueLabel(dlg, "propertiesAvailableValue")->text().contains("12.0 GB"));
    EXPECT_EQ(dlg.findChild<QGroupBox *>(QStringLiteral("propertiesPermissionsGroup")), nullptr);
}

TEST(PropertiesDialogTest, RemovableDeviceMetadataLabelsAreLocalized) {
    TranslationManager::switchTo(*qApp, QStringLiteral("zh_CN"));

    RemovableDevice device;
    device.name = QStringLiteral("Backup USB");
    device.mountPoint = QStringLiteral("E:\\");
    device.bytesTotal = 64LL * 1024 * 1024 * 1024;
    device.bytesAvailable = 12LL * 1024 * 1024 * 1024;
    PropertiesDialog dlg(device);

    EXPECT_TRUE(showsText(dlg, QStringLiteral("可移动设备")));
    EXPECT_TRUE(showsText(dlg, QStringLiteral("总容量：")));
    EXPECT_TRUE(showsText(dlg, QStringLiteral("可用空间：")));
    EXPECT_FALSE(showsText(dlg, QStringLiteral("Removable device")));
    EXPECT_FALSE(showsText(dlg, QStringLiteral("Total capacity:")));
    EXPECT_FALSE(showsText(dlg, QStringLiteral("Available space:")));

    TranslationManager::switchTo(*qApp, QStringLiteral("en"));
}

TEST(PropertiesDialogTest, RemovableDeviceShowsUnavailableStorageMetricsExplicitly) {
    RemovableDevice device;
    device.mountPoint = QStringLiteral("E:\\");

    PropertiesDialog dlg(device);

    EXPECT_EQ(valueLabel(dlg, "propertiesNameValue")->text(), QStringLiteral("Unavailable"));
    EXPECT_EQ(valueLabel(dlg, "propertiesCapacityValue")->text(), QStringLiteral("Unavailable"));
    EXPECT_EQ(valueLabel(dlg, "propertiesAvailableValue")->text(), QStringLiteral("Unavailable"));
}

TEST(PropertiesDialogTest, RemovableDeviceAcceptsWithoutPermissionControls) {
    RemovableDevice device;
    device.name = QStringLiteral("Backup USB");

    PropertiesDialog dlg(device);
    auto *buttons = dlg.findChild<QDialogButtonBox *>();
    ASSERT_NE(buttons, nullptr);

    buttons->button(QDialogButtonBox::Ok)->click();

    EXPECT_EQ(dlg.result(), QDialog::Accepted);
}

// --- Permission tri-state --------------------------------------------------

TEST(PropertiesDialogTest, PermissionStatesAgreeAndDisagreeAcrossEntries) {
    const QFile::Permissions p644 = PropertiesDialog::fromOctal(0644);
    const QFile::Permissions p600 = PropertiesDialog::fromOctal(0600);

    const QVector<Qt::CheckState> same = PropertiesDialog::permissionStates({p644, p644});
    ASSERT_EQ(same.size(), 9);
    EXPECT_EQ(same[0], Qt::Checked);    // owner read
    EXPECT_EQ(same[2], Qt::Unchecked);  // owner execute
    EXPECT_EQ(same[3], Qt::Checked);    // group read

    const QVector<Qt::CheckState> mixed = PropertiesDialog::permissionStates({p644, p600});
    EXPECT_EQ(mixed[0], Qt::Checked);           // both allow owner read
    EXPECT_EQ(mixed[3], Qt::PartiallyChecked);  // they disagree on group read
    EXPECT_EQ(mixed[6], Qt::PartiallyChecked);  // ... and on other read
}

TEST(PropertiesDialogTest, PermissionStatesOfNothingReadAsAllClear) {
    const QVector<Qt::CheckState> states = PropertiesDialog::permissionStates({});
    ASSERT_EQ(states.size(), 9);
    for (Qt::CheckState s : states)
        EXPECT_EQ(s, Qt::Unchecked);
}

// --- Permission editing is refused for provider-backed entries -------------
//
// The bug: the boxes were filled from the provider's real bits (say 744) while
// the "original" they were diffed against came from a local stat of the server's
// path (0), so OK always tried a chmod. It failed noisily on a name with no
// local counterpart -- and succeeded, on the WRONG file, when one existed.

TEST(PropertiesDialogTest, RemoteEntriesShowPermissionsButCannotEditThem) {
    PropertiesDialog dlg(
        QVector<FileInfo>{remoteFile("/share/a.pdf", 1024, PropertiesDialog::fromOctal(0744))});
    const QList<QCheckBox *> boxes = dlg.findChildren<QCheckBox *>();
    ASSERT_EQ(boxes.size(), 9);
    for (const QCheckBox *box : boxes)
        EXPECT_FALSE(box->isEnabled()) << "a server's permission bits are not ours to chmod";
    // Still shown, though -- the point of the grid is to report what the server
    // says, and 0744 is owner-rwx / group-r / other-r.
    EXPECT_TRUE(showsText(dlg, QStringLiteral("744")));
}

TEST(PropertiesDialogTest, LocalEntriesUsePlatformAppropriatePermissionControls) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = QDir(dir.path()).filePath("local.txt");
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("x");
    f.close();

    PropertiesDialog dlg(path);
    const QList<QCheckBox *> boxes = dlg.findChildren<QCheckBox *>();
#ifdef Q_OS_WIN
    EXPECT_TRUE(boxes.isEmpty());
#else
    ASSERT_EQ(boxes.size(), 9);
    for (const QCheckBox *box : boxes)
        EXPECT_TRUE(box->isEnabled());
#endif
}

TEST(PropertiesDialogTest, OkOnARemoteEntryClosesWithoutTouchingASameNamedLocalFile) {
    // A local file sitting at exactly the path the server reports -- the trap
    // that made this a data-corruption bug and not merely an error dialog.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = QDir(dir.path()).filePath("report.pdf");
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("local bytes");
    f.close();
    ASSERT_TRUE(QFile::setPermissions(path, PropertiesDialog::fromOctal(0600)));
    const QFileDevice::Permissions originalPermissions = QFileInfo(path).permissions();

    // The remote entry of the same name, with different bits.
    PropertiesDialog dlg(QVector<FileInfo>{remoteFile(path, 4096, PropertiesDialog::fromOctal(0744))});
    auto *buttons = dlg.findChild<QDialogButtonBox *>();
    ASSERT_NE(buttons, nullptr);
    buttons->button(QDialogButtonBox::Ok)->click();

    EXPECT_EQ(dlg.result(), QDialog::Accepted) << "OK must close the dialog, not error out";
    EXPECT_EQ(QFileInfo(path).permissions(), originalPermissions)
        << "the local file that happens to share the remote name must be left alone";
}
