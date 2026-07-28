#include <gtest/gtest.h>

#include <QCheckBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>
#include <QVector>

#include "FileInfo.h"
#include "dialogs/PropertiesDialog.h"

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

} // namespace

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

TEST(PropertiesDialogTest, LocalEntriesKeepEditablePermissions) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = QDir(dir.path()).filePath("local.txt");
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("x");
    f.close();

    PropertiesDialog dlg(path);
    const QList<QCheckBox *> boxes = dlg.findChildren<QCheckBox *>();
    ASSERT_EQ(boxes.size(), 9);
    for (const QCheckBox *box : boxes)
        EXPECT_TRUE(box->isEnabled());
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
