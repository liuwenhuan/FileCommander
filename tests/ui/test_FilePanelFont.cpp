#include <gtest/gtest.h>

#include <QDir>
#include <QApplication>
#include <QFile>
#include <QLineEdit>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeView>

#include "FilePanel.h"
#include "FileListView.h"
#include "FileSystemModel.h"
#include "tree/DirectoryTreeModel.h"

// The folder tree and the inline-rename editor both have to track the View-menu
// font size, and both silently missed it. Neither is reachable through a public
// accessor, so drive the widgets directly here.
namespace {

// Waits for the panel's asynchronous directory load to land.
void settle(FilePanel &panel, const QString &path) {
    QSignalSpy spy(panel.model(), &FileSystemModel::loadFinished);
    panel.navigateTo(path);
    if (spy.isEmpty())
        spy.wait(4000);
    QCoreApplication::processEvents();
}

// The tree has no accessor on FilePanel, so find it by its model type: it is
// the panel's only QTreeView over a DirectoryTreeModel.
QTreeView *showTree(FilePanel &panel) {
    for (QTreeView *t : panel.findChildren<QTreeView *>())
        if (qobject_cast<DirectoryTreeModel *>(t->model())) {
            t->setVisible(true);
            return t;
        }
    if (auto *button = panel.findChild<QToolButton *>(QStringLiteral("PanelTreeButton")))
        button->click();
    for (QTreeView *t : panel.findChildren<QTreeView *>())
        if (qobject_cast<DirectoryTreeModel *>(t->model()))
            return t;
    return nullptr;
}

class FilePanelFontTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(m_dir.isValid());
        const QDir root(m_dir.path());
        QFile f(root.filePath("gamma.txt"));
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("x");
    }
    QTemporaryDir m_dir;
};

TEST_F(FilePanelFontTest, TreeFontFollowsListFontSize) {
    FilePanel panel;
    QTreeView *tree = showTree(panel);
    ASSERT_NE(tree, nullptr);

    panel.setListFontSize(17);
    EXPECT_EQ(tree->font().pointSize(), 17);
    // The editor is a child of the viewport, so the viewport must carry the size
    // too -- setting it on the view alone is what the bug was.
    EXPECT_EQ(tree->viewport()->font().pointSize(), 17);

    panel.setListFontSize(9); // live change, no restart
    EXPECT_EQ(tree->font().pointSize(), 9);
    EXPECT_EQ(tree->viewport()->font().pointSize(), 9);
}

TEST_F(FilePanelFontTest, RenameEditorUsesListFontSize) {
    // The theme stylesheet is what makes this fail: QStyleSheetStyle::polish()
    // (run by FileListView's selection-palette probing, and again on every theme
    // switch) re-resolves the VIEWPORT font from the global stylesheet and clears
    // its resolve mask, so the editor -- a child of the viewport -- stops
    // inheriting the size set on the view. Without a stylesheet loaded here, the
    // plain-inheritance path works and the bug is invisible.
    // Read from the source tree: resources.qrc is linked into the app target,
    // not into this test binary, so ":/themes/dark.qss" does not resolve here.
    QFile qss(QStringLiteral(TTC_SOURCE_DIR "/resources/themes/dark.qss"));
    ASSERT_TRUE(qss.open(QIODevice::ReadOnly)) << "theme qss missing";
    qApp->setStyleSheet(QString::fromUtf8(qss.readAll()));
    // Leave the app clean for whatever test runs next.
    struct ClearSheet {
        ~ClearSheet() { qApp->setStyleSheet(QString()); }
    } clearSheet;

    FilePanel panel;
    panel.show();
    settle(panel, m_dir.path());
    // Force the polish cycle that severs the viewport's font inheritance, so the
    // font is applied against the same widget state the real app has.
    panel.view()->setPanelActive(true);
    panel.setListFontSize(16);

    FileListView *view = panel.view();
    // ".." is not editable, so rename must be aimed at a real entry.
    int row = -1;
    for (int r = 0; r < panel.model()->rowCount(); ++r)
        if (!panel.model()->fileInfoAt(r).isParentEntry()) {
            row = r;
            break;
        }
    ASSERT_GE(row, 0);
    const QModelIndex idx = panel.model()->index(row, FileSystemModel::NameColumn);
    view->setCurrentIndex(idx);
    view->edit(idx);

    const QList<QLineEdit *> editors = view->viewport()->findChildren<QLineEdit *>();
    ASSERT_FALSE(editors.isEmpty()) << "no inline editor was created";
    EXPECT_EQ(editors.first()->font().pointSize(), 16);
}

} // namespace
