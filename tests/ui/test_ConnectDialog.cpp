#include <gtest/gtest.h>

#include "dialogs/ConnectDialog.h"

#include <QComboBox>

TEST(ConnectDialog, OffersOnlyBackedProtocols) {
    ConnectDialog dialog;
    const auto *protocols = dialog.findChild<QComboBox *>(QStringLiteral("protocolCombo"));
    ASSERT_NE(protocols, nullptr);

    QStringList labels;
    for (int i = 0; i < protocols->count(); ++i)
        labels.append(protocols->itemText(i));

    EXPECT_TRUE(labels.contains(QStringLiteral("SFTP (SSH)")));
    EXPECT_TRUE(labels.contains(QStringLiteral("FTP")));
    EXPECT_TRUE(labels.contains(QStringLiteral("WebDAV (HTTP)")));
    EXPECT_TRUE(labels.contains(QStringLiteral("WebDAV (HTTPS)")));
#if FILECOMMANDER_HAS_LINUX_INTEGRATION
    EXPECT_TRUE(labels.contains(QStringLiteral("SMB / Windows share")));
#else
    EXPECT_FALSE(labels.contains(QStringLiteral("SMB / Windows share")));
#endif
}
