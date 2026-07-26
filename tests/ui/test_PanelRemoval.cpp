#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <memory>

#include "FilePanel.h"
#include "FileProvider.h"
#include "FileSystemModel.h"

// What the listing does once a delete or a move has finished.
//
// The rows to drop were chosen with QFileInfo::exists(), which answers about
// THIS machine. On a network tab that is a different file: every remote path
// with no local namesake read as "gone", so the whole selection vanished from
// the listing whether or not the server had actually removed it -- and the
// source panel was deliberately left unrefreshed, so nothing corrected it.
namespace {

// A server whose contents the test controls. Counts list() calls so "it went
// back and asked the server" can be asserted rather than assumed.
class FakeShare : public FileProvider {
public:
    QString displayName() const override { return QStringLiteral("tester@share"); }
    QString scheme() const override { return QStringLiteral("smb"); }

    QVector<FileInfo> list(const QString &path, bool) const override {
        ++listCalls;
        QVector<FileInfo> out;
        for (const QString &name : names)
            out.append(FileInfo::fromFields(path + QLatin1Char('/') + name, name, 10,
                                            QDateTime::fromSecsSinceEpoch(1000000), false,
                                            QFile::ReadOwner));
        return out;
    }
    bool isDir(const QString &) const override { return true; }
    QString cleanPath(const QString &p) const override { return p; }
    QString parentPath(const QString &path) const override {
        const int slash = path.lastIndexOf(QLatin1Char('/'));
        if (slash < 0 || path == QLatin1String("/"))
            return {};
        return slash == 0 ? QStringLiteral("/") : path.left(slash);
    }
    bool exists(const QString &) const override { return true; }
    RenameResult rename(const QString &, const QString &, QString *) override {
        return RenameResult::Failed;
    }

    QStringList names;
    mutable int listCalls = 0;
};

void settle(FilePanel &panel) {
    QSignalSpy spy(panel.model(), &FileSystemModel::loadFinished);
    if (spy.isEmpty())
        spy.wait(4000);
    QCoreApplication::processEvents();
}

bool listHasName(FileSystemModel *model, const QString &name) {
    for (int row = 0; row < model->rowCount(); ++row)
        if (model->fileInfoAt(row).name() == name)
            return true;
    return false;
}

void touch(const QString &path) {
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write("x");
    f.close();
}

} // namespace

TEST(PanelRemovalTest, LocalDeleteDropsExactlyTheFilesThatWent) {
    // The local behaviour, unchanged: the deleted rows go, the survivor stays,
    // and the listing is not rescanned out from under the cursor.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString gone = QDir(dir.path()).filePath(QStringLiteral("gone.txt"));
    const QString kept = QDir(dir.path()).filePath(QStringLiteral("kept.txt"));
    touch(gone);
    touch(kept);

    FilePanel panel;
    panel.navigateTo(dir.path());
    settle(panel);
    ASSERT_TRUE(listHasName(panel.model(), QStringLiteral("gone.txt")));

    ASSERT_TRUE(QFile::remove(gone));
    panel.settleAfterRemoval({gone});
    QCoreApplication::processEvents();

    EXPECT_FALSE(listHasName(panel.model(), QStringLiteral("gone.txt")));
    EXPECT_TRUE(listHasName(panel.model(), QStringLiteral("kept.txt")));
}

TEST(PanelRemovalTest, LocalDeleteThatFailedKeepsTheRow) {
    // A file the delete could not remove is still there, and must still be
    // listed -- the row is the only sign the operation did not do what it said.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString survivor = QDir(dir.path()).filePath(QStringLiteral("survivor.txt"));
    touch(survivor);

    FilePanel panel;
    panel.navigateTo(dir.path());
    settle(panel);

    panel.settleAfterRemoval({survivor}); // the file was never actually removed
    QCoreApplication::processEvents();
    EXPECT_TRUE(listHasName(panel.model(), QStringLiteral("survivor.txt")));
}

TEST(PanelRemovalTest, RemoteDeleteAsksTheServerInsteadOfGuessingLocally) {
    // "/share/docs/report.pdf" has no namesake on this machine, so the old test
    // read "gone" and dropped the row -- for a file the server still holds.
    auto share = std::make_shared<FakeShare>();
    share->names = QStringList{QStringLiteral("report.pdf"), QStringLiteral("notes.txt")};

    FilePanel panel;
    panel.connectTabTo(0, share, [](QString *) { return true; },
                       QStringLiteral("/share/docs"), QStringLiteral("tester@share"),
                       SavedConnection{}, FileSystemModel::AuthRetryFactory());
    settle(panel);
    ASSERT_TRUE(listHasName(panel.model(), QStringLiteral("report.pdf")));

    // The delete "finished", but the server refused this one: it is still there.
    const int listsBefore = share->listCalls;
    panel.settleAfterRemoval({QStringLiteral("/share/docs/report.pdf")});
    settle(panel);

    EXPECT_GT(share->listCalls, listsBefore) << "never went back to the server";
    EXPECT_TRUE(listHasName(panel.model(), QStringLiteral("report.pdf")))
        << "dropped a row for a file the server still has";
    EXPECT_TRUE(listHasName(panel.model(), QStringLiteral("notes.txt")));
}

TEST(PanelRemovalTest, RemoteDeleteThatSucceededDropsTheRow) {
    // And when the server really did remove it, the relist reflects that.
    auto share = std::make_shared<FakeShare>();
    share->names = QStringList{QStringLiteral("report.pdf"), QStringLiteral("notes.txt")};

    FilePanel panel;
    panel.connectTabTo(0, share, [](QString *) { return true; },
                       QStringLiteral("/share/docs"), QStringLiteral("tester@share"),
                       SavedConnection{}, FileSystemModel::AuthRetryFactory());
    settle(panel);

    share->names = QStringList{QStringLiteral("notes.txt")}; // the server removed it
    panel.settleAfterRemoval({QStringLiteral("/share/docs/report.pdf")});
    settle(panel);

    EXPECT_FALSE(listHasName(panel.model(), QStringLiteral("report.pdf")));
    EXPECT_TRUE(listHasName(panel.model(), QStringLiteral("notes.txt")));
}
