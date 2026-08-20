#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QKeySequence>
#include <QShortcut>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include "FilePanel.h"
#include "FileSystemModel.h"
#include "MainWindow.h"
#include "ThemeStateGuard.h"
#include "TryUntil.h"
#include "dialogs/DeleteConfirmDialog.h"
#include "operations/OperationQueue.h"

// Which panels a delete has to re-list.
//
// The panel the delete happened in settles itself; the OTHER one used to be
// refreshed unconditionally, on the grounds that it *might* be showing the same
// directory. Most of the time it is not, and the relist then throws away that
// panel's scroll position and selection for a listing nothing touched -- on a
// network or archive tab at the cost of a round trip as well.
namespace {

void settle(FilePanel *panel) {
    QSignalSpy loaded(panel->model(), &FileSystemModel::loadFinished);
    if (loaded.isEmpty())
        loaded.wait(4000);
    qApp->processEvents();
}

void touch(const QString &path) {
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write("x");
    file.close();
}

// Shift+Delete always confirms -- a permanent delete cannot be undone -- so the
// dialog has to be answered from outside the nested exec() it runs in.
class ConfirmAccepter {
public:
    ConfirmAccepter() {
        QObject::connect(&m_timer, &QTimer::timeout, [this] {
            if (auto *dialog = qobject_cast<DeleteConfirmDialog *>(qApp->activeModalWidget())) {
                ++m_accepted;
                dialog->accept();
            }
        });
        m_timer.start(20);
    }
    int accepted() const { return m_accepted; }

private:
    QTimer m_timer;
    int m_accepted = 0;
};

// Permanently deletes `file` from `source`, and reports how many times `other`
// re-listed while it happened.
int otherPanelReloadsWhileDeleting(MainWindow &window, FilePanel *source, FilePanel *other,
                                   const QString &file) {
    source->selectPathAfterReload(file);
    source->refresh();
    settle(source);
    if (source->selectedPaths() != QStringList{file})
        return -1; // nothing selected: the delete below would act on the cursor
    window.setActivePanel(source);

    auto *queue = window.findChild<OperationQueue *>();
    if (!queue)
        return -1;
    QSignalSpy finished(queue, &OperationQueue::finished);
    QSignalSpy otherLoads(other->model(), &FileSystemModel::loadFinished);

    ConfirmAccepter accepter;
    QShortcut *shiftDelete = nullptr;
    for (QShortcut *shortcut : window.findChildren<QShortcut *>())
        if (shortcut->key() == QKeySequence(Qt::SHIFT | Qt::Key_Delete))
            shiftDelete = shortcut;
    if (!shiftDelete)
        return -1;
    if (!QMetaObject::invokeMethod(shiftDelete, "activated", Qt::DirectConnection))
        return -1;

    if (finished.isEmpty())
        finished.wait(5000);
    // Give a refresh that the finished handler asked for time to land -- and an
    // unwanted one time to show itself.
    QTest::qWait(300);
    return accepter.accepted() == 1 ? otherLoads.count() : -1;
}

} // namespace

TEST(PanelDeleteRefreshTest, PanelOnAnotherDirectoryIsLeftAlone) {
    ThemeStateGuard themeState;
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString sourceDir = QDir(root.path()).filePath(QStringLiteral("here"));
    const QString elsewhere = QDir(root.path()).filePath(QStringLiteral("elsewhere"));
    ASSERT_TRUE(QDir().mkpath(sourceDir));
    ASSERT_TRUE(QDir().mkpath(elsewhere));
    const QString doomed = QDir(sourceDir).filePath(QStringLiteral("doomed.txt"));
    touch(doomed);
    touch(QDir(elsewhere).filePath(QStringLiteral("untouched.txt")));

    MainWindow window;
    const QList<FilePanel *> panels = window.findChildren<FilePanel *>();
    ASSERT_GE(panels.size(), 2);
    FilePanel *left = panels.at(0);
    FilePanel *right = panels.at(1);
    left->navigateTo(sourceDir);
    settle(left);
    right->navigateTo(elsewhere);
    settle(right);

    const int reloads = otherPanelReloadsWhileDeleting(window, left, right, doomed);
    EXPECT_FALSE(QFile::exists(doomed));
    EXPECT_EQ(reloads, 0) << "re-listed a directory the delete never touched";
}

TEST(PanelDeleteRefreshTest, PanelShowingTheSameDirectoryIsRefreshed) {
    ThemeStateGuard themeState;
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString shared = QDir(root.path()).filePath(QStringLiteral("shared"));
    ASSERT_TRUE(QDir().mkpath(shared));
    const QString doomed = QDir(shared).filePath(QStringLiteral("doomed.txt"));
    touch(doomed);
    touch(QDir(shared).filePath(QStringLiteral("kept.txt")));

    MainWindow window;
    const QList<FilePanel *> panels = window.findChildren<FilePanel *>();
    ASSERT_GE(panels.size(), 2);
    FilePanel *left = panels.at(0);
    FilePanel *right = panels.at(1);
    left->navigateTo(shared);
    settle(left);
    right->navigateTo(shared);
    settle(right);

    const int reloads = otherPanelReloadsWhileDeleting(window, left, right, doomed);
    EXPECT_FALSE(QFile::exists(doomed));
    EXPECT_GE(reloads, 1) << "left the deleted file listed in the other panel";

    for (int row = 0; row < right->model()->rowCount(); ++row)
        EXPECT_NE(right->model()->fileInfoAt(row).name(), QStringLiteral("doomed.txt"));
}
