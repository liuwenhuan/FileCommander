#include <gtest/gtest.h>

#include <QDropEvent>
#include <QMimeData>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>

#include <memory>

#include "ExternalPaths.h"
#include "FileListView.h"
#include "FileProvider.h"
#include "FileSystemModel.h"
#include "IconFileView.h"
#include "LocalFileProvider.h"

// A drag out of a panel now carries two payloads: file:// URLs (or protocol
// URIs) for other applications, and FileCommander's own path list for its own
// drops. These tests cover the second half -- that in-app drag and drop still
// lands the *backend's* paths, including for sources whose public payload is
// deliberately empty (an archive entry has no name outside this process).
namespace {

// A backend whose paths belong to a server, not to this machine.
class FakeRemote : public FileProvider {
public:
    QString displayName() const override { return QStringLiteral("deepin@nas.invalid"); }
    QString scheme() const override { return QStringLiteral("smb"); }
    RemoteLocation remoteLocation() const override {
        RemoteLocation loc;
        loc.scheme = QStringLiteral("smb");
        loc.host = QStringLiteral("nas.invalid");
        loc.user = QStringLiteral("alice");
        return loc;
    }
    QVector<FileInfo> list(const QString &, bool) const override { return {}; }
    bool isDir(const QString &) const override { return true; }
    QString cleanPath(const QString &p) const override { return p; }
    QString parentPath(const QString &) const override { return {}; }
    bool exists(const QString &) const override { return true; }
    RenameResult rename(const QString &, const QString &, QString *) override {
        return RenameResult::Failed;
    }
};

// An archive backend: its "/etc/passwd" names an entry inside an archive file.
class FakeArchive : public FakeRemote {
public:
    QString displayName() const override { return {}; }
    QString scheme() const override { return {}; }
    RemoteLocation remoteLocation() const override { return {}; }
};

// dropEvent is protected; a drop is otherwise only reachable from a real drag
// loop, which a test cannot drive.
class DroppableView : public FileListView {
public:
    using FileListView::dragEnterEvent;
    using FileListView::dropEvent;
};

class DroppableIconView : public IconFileView {
public:
    using IconFileView::dragEnterEvent;
    using IconFileView::dropEvent;
};

// filesDropped carries an enum and a raw provider pointer; QSignalSpy warns on
// every emission unless both are known to the meta-object system.
void registerSignalTypes() {
    static const bool once = [] {
        qRegisterMetaType<FileListView::DropActionKind>("FileListView::DropActionKind");
        qRegisterMetaType<FileProvider *>("FileProvider*");
        return true;
    }();
    Q_UNUSED(once);
}

// A view showing a real local directory, so destinationDirForDrop() has a
// destination to report.
template <typename View>
struct DropPanel {
    QTemporaryDir dir;
    FileSystemModel model;
    View view;

    DropPanel() {
        registerSignalTypes();
        model.setRootPath(dir.path());
        view.setModel(&model);
        view.resize(400, 300);
    }
};

using Panel = DropPanel<DroppableView>;
using IconPanel = DropPanel<DroppableIconView>;

template <typename View>
bool sendDrop(View *view, QMimeData *mime) {
    QDropEvent event(QPoint(5, 5), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
    view->dropEvent(&event);
    return event.isAccepted();
}

} // namespace

TEST(DragPayloadTest, InAppDropCarriesTheBackendPathsNotUrls) {
    Panel panel;
    QSignalSpy spy(&panel.view, &FileListView::filesDropped);

    FakeRemote remote;
    QMimeData mime;
    const QStringList paths = {QStringLiteral("/docs/report.pdf")};
    fc::setPathPayload(&mime, &remote, paths, /*cut=*/false);
    sendDrop(&panel.view, &mime);

    ASSERT_EQ(spy.count(), 1) << "an in-app drag from a network panel was refused";
    EXPECT_EQ(spy.at(0).at(0).toStringList(), paths);
}

TEST(DragPayloadTest, ArchiveEntryDropStillWorksWithNoPublicUrls) {
    // The public payload is empty for an archive source (nothing outside this
    // process can open an in-archive entry), so the drop has nothing but the
    // private format to go on. It has to be enough -- copying files out of an
    // archive by dragging them is exactly what this is for.
    Panel panel;
    QSignalSpy spy(&panel.view, &FileListView::filesDropped);

    FakeArchive archive;
    QMimeData mime;
    const QStringList paths = {QStringLiteral("/etc/passwd"), QStringLiteral("/README.md")};
    fc::setPathPayload(&mime, &archive, paths, /*cut=*/false);
    ASSERT_FALSE(mime.hasUrls());

    // The drag must be accepted on the way in, too: the old handlers gated on
    // hasUrls() and would have rejected this before the drop ever happened.
    QDragEnterEvent enter(QPoint(5, 5), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    panel.view.dragEnterEvent(&enter);
    EXPECT_TRUE(enter.isAccepted());

    sendDrop(&panel.view, &mime);
    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(0).toStringList(), paths);
}

TEST(DragPayloadTest, DropFromAnotherApplicationStillReadsFileUrls) {
    // Regression guard for the local/external half: a drag from Nautilus carries
    // nothing but file:// URLs, and must keep working exactly as before.
    Panel panel;
    QSignalSpy spy(&panel.view, &FileListView::filesDropped);

    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(QStringLiteral("/tmp/one.txt")),
                  QUrl::fromLocalFile(QStringLiteral("/tmp/two.txt"))});
    sendDrop(&panel.view, &mime);

    ASSERT_EQ(spy.count(), 1);
    const QStringList got = spy.at(0).at(0).toStringList();
    ASSERT_EQ(got.size(), 2);
    EXPECT_EQ(got.at(0).toStdString(), "/tmp/one.txt");
    EXPECT_EQ(got.at(1).toStdString(), "/tmp/two.txt");
}

TEST(DragPayloadTest, LocalDragStillAdvertisesPlainFileUrls) {
    // What another application sees when the source is an ordinary local panel:
    // unchanged file:// URLs, plus the private copy for our own drops.
    QMimeData mime;
    const QStringList paths = {QStringLiteral("/home/alice/a.txt")};
    fc::setPathPayload(&mime, LocalFileProvider::instance(), paths, /*cut=*/false);

    ASSERT_TRUE(mime.hasUrls());
    ASSERT_EQ(mime.urls().size(), 1);
    EXPECT_EQ(mime.urls().first().toString().toStdString(), "file:///home/alice/a.txt");
    EXPECT_EQ(fc::incomingPaths(&mime), paths);
}

TEST(DragPayloadTest, DropWithNothingUsableIsIgnored) {
    Panel panel;
    QSignalSpy spy(&panel.view, &FileListView::filesDropped);

    QMimeData mime;
    mime.setText(QStringLiteral("just some text"));
    sendDrop(&panel.view, &mime);
    EXPECT_EQ(spy.count(), 0);
}

TEST(DragPayloadTest, IconViewPrivatePayloadCarriesBackendPathsAndCopyAction) {
    IconPanel panel;
    QSignalSpy spy(&panel.view, &IconFileView::filesDropped);

    FakeRemote remote;
    QMimeData mime;
    const QStringList paths = {QStringLiteral("/docs/report.pdf"),
                               QStringLiteral("/docs/data.csv")};
    fc::setPathPayload(&mime, &remote, paths, /*cut=*/false);
    ASSERT_TRUE(sendDrop(&panel.view, &mime));

    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(0).toStringList(), paths);
    EXPECT_EQ(qvariant_cast<FileListView::DropActionKind>(spy.at(0).at(2)),
              FileListView::DropActionKind::Copy);
}

TEST(DragPayloadTest, IconViewExternalUrlsRemainUsable) {
    IconPanel panel;
    QSignalSpy spy(&panel.view, &IconFileView::filesDropped);

    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(QStringLiteral("/tmp/one.txt")),
                  QUrl::fromLocalFile(QStringLiteral("/tmp/two.txt"))});
    ASSERT_TRUE(sendDrop(&panel.view, &mime));

    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(0).toStringList(),
              QStringList({QStringLiteral("/tmp/one.txt"), QStringLiteral("/tmp/two.txt")}));
    EXPECT_EQ(qvariant_cast<FileListView::DropActionKind>(spy.at(0).at(2)),
              FileListView::DropActionKind::Copy);
}

TEST(DragPayloadTest, IconViewExternalDropSelectsCopyAction) {
    IconPanel panel;
    QSignalSpy spy(&panel.view, &IconFileView::filesDropped);

    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(QStringLiteral("/tmp/copy-me.txt"))});
    ASSERT_TRUE(sendDrop(&panel.view, &mime));

    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(qvariant_cast<FileListView::DropActionKind>(spy.at(0).at(2)),
              FileListView::DropActionKind::Copy);
}

TEST(DragPayloadTest, IconViewDropWithNothingUsableIsIgnored) {
    IconPanel panel;
    QSignalSpy spy(&panel.view, &IconFileView::filesDropped);

    QMimeData mime;
    mime.setText(QStringLiteral("just some text"));
    EXPECT_FALSE(sendDrop(&panel.view, &mime));
    EXPECT_EQ(spy.count(), 0);
}
