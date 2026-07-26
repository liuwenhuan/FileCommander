#include <gtest/gtest.h>

#include <QLabel>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <memory>

#include "FilePanel.h"
#include "FileProvider.h"
#include "FileSystemModel.h"

// The "N free of M" readout in the status strip.
//
// It was built from QStorageInfo(rootPath()), which answers about THIS
// machine's mount table. On a network tab that produced a number for the local
// partition whose mount point happened to match the server's path -- a share
// browsing "/home" reported this machine's /home, byte for byte identical to
// the local panel beside it, while the server's real capacity never appeared.
namespace {

class FakeShare : public FileProvider {
public:
    QString displayName() const override { return QStringLiteral("tester@share"); }
    QString scheme() const override { return QStringLiteral("smb"); }
    QVector<FileInfo> list(const QString &, bool) const override { return {}; }
    bool isDir(const QString &) const override { return true; }
    QString cleanPath(const QString &p) const override { return p; }
    QString parentPath(const QString &path) const override {
        const int slash = path.lastIndexOf(QLatin1Char('/'));
        if (slash < 0)
            return {};
        return slash == 0 ? QStringLiteral("/") : path.left(slash);
    }
    bool exists(const QString &) const override { return true; }
    RenameResult rename(const QString &, const QString &, QString *) override {
        return RenameResult::Failed;
    }
};

void settle(FilePanel &panel) {
    QSignalSpy spy(panel.model(), &FileSystemModel::loadFinished);
    if (spy.isEmpty())
        spy.wait(4000);
    QCoreApplication::processEvents();
}

// The status strip's disk readout, or an empty string when it is blank. Found
// by its wording rather than by widget order, which is what the user reads too.
QString diskReadout(FilePanel &panel) {
    for (QLabel *label : panel.findChildren<QLabel *>())
        if (label->text().contains(QStringLiteral("free of")))
            return label->text();
    return {};
}

} // namespace

TEST(PanelDiskInfoTest, LocalTabStillReportsTheDisk) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    FilePanel panel;
    panel.navigateTo(dir.path());
    settle(panel);
    EXPECT_FALSE(diskReadout(panel).isEmpty())
        << "the local disk readout is the behaviour that must not change";
}

TEST(PanelDiskInfoTest, NetworkTabReportsNothingRatherThanTheLocalDisk) {
    FilePanel panel;
    // "/home" on purpose: it is a mount point on this machine as well, which is
    // exactly the case where QStorageInfo silently answered about the wrong one.
    panel.connectTabTo(0, std::make_shared<FakeShare>(), [](QString *) { return true; },
                       QStringLiteral("/home"), QStringLiteral("tester@share"),
                       SavedConnection{}, FileSystemModel::AuthRetryFactory());
    settle(panel);
    ASSERT_TRUE(panel.model()->hasNetworkSession());
    EXPECT_TRUE(diskReadout(panel).isEmpty())
        << "reported " << diskReadout(panel).toStdString() << " for a directory on a server";
}

TEST(PanelDiskInfoTest, ReadoutComesBackWhenTheTabReturnsToLocal) {
    // Blanking must be tied to what the tab is showing now, not latched.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    FilePanel panel;
    panel.connectTabTo(0, std::make_shared<FakeShare>(), [](QString *) { return true; },
                       QStringLiteral("/home"), QStringLiteral("tester@share"),
                       SavedConnection{}, FileSystemModel::AuthRetryFactory());
    settle(panel);
    ASSERT_TRUE(diskReadout(panel).isEmpty());

    panel.openLocalInTab(-1, dir.path());
    settle(panel);
    EXPECT_FALSE(diskReadout(panel).isEmpty());
}
