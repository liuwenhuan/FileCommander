#include <gtest/gtest.h>

#include <QFile>

#include "dialogs/PropertiesDialog.h"

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
