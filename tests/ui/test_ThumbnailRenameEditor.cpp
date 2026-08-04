#include <gtest/gtest.h>

#include <QDir>
#include <QFontMetrics>
#include <QElapsedTimer>
#include <QFile>
#include <QLineEdit>
#include <QListView>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "FileSystemModel.h"
#include "IconFileView.h"
#include "ThumbnailDelegate.h"

// Renaming in the thumbnail grid opened an editor that did not belong to the
// cell it was editing. Two measured symptoms, both visible at a large zoom:
//
//   * It was drawn in the VIEW's font (12pt in the setup below) while the label
//     it replaces is drawn in the delegate's (18pt) -- small type in a big tile.
//   * QStyledItemDelegate places it with the style's SE_ItemViewItemText, which
//     put it at y=195 in a cell whose label starts at y=211: the box rode up
//     over the bottom of the icon and the label text showed around it.
//
// The delegate lays this cell out itself, so it has to place and size the
// editor itself too. Driving it through a real view matters: without one,
// initStyleOption fills in nothing, the style's text sub-rect degenerates to
// the whole cell, and the old behaviour measures as correct.

namespace {

void pump(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

// One directory holding one long-named file, shown in a thumbnail grid built
// the way FilePanel builds it (see FilePanel::applyThumbnailIconSize).
struct Grid {
    QTemporaryDir dir;
    FileSystemModel model;
    IconFileView view;
    ThumbnailDelegate *delegate = nullptr;

    explicit Grid(int iconPx, int labelPt) {
        QFile file(QDir(dir.path()).filePath(
            QStringLiteral("a-rather-long-file-name-that-has-to-wrap.txt")));
        file.open(QIODevice::WriteOnly);
        file.write("x");
        file.close();

        view.setModel(&model);
        view.setViewMode(QListView::IconMode);
        delegate = new ThumbnailDelegate(&view);
        delegate->setView(&view);
        delegate->setFontPointSize(labelPt);
        view.setItemDelegate(delegate);

        view.setIconSize(QSize(iconPx, iconPx));
        delegate->setIconSize(iconPx);
        const QSize cell = delegate->cellSizeHint(view.font());
        view.setGridSize(QSize(cell.width() + 26, cell.height()));

        view.resize(cell.width() * 3, cell.height() * 3);
        view.show();

        QSignalSpy loaded(&model, &FileSystemModel::loadFinished);
        model.setRootPath(dir.path());
        if (loaded.isEmpty())
            loaded.wait(5000);
        view.doItemsLayout();
        pump(100);
    }

    QModelIndex fileIndex() const {
        for (int row = 0; row < model.rowCount(); ++row) {
            const QModelIndex index = model.index(row, FileSystemModel::NameColumn);
            if (!model.isParentEntry(row))
                return index;
        }
        return {};
    }

    // The editor the view opens for an inline rename, already placed.
    QLineEdit *openEditor() {
        const QModelIndex index = fileIndex();
        if (!index.isValid())
            return nullptr;
        view.setCurrentIndex(index);
        view.edit(index);
        pump(50);
        return view.viewport()->findChild<QLineEdit *>();
    }
};

} // namespace

TEST(ThumbnailRenameEditor, TheEditorCoversTheLabelItIsReplacing) {
    Grid grid(192, 18);
    QLineEdit *editor = grid.openEditor();
    ASSERT_NE(editor, nullptr);

    const QRect cell = grid.view.visualRect(grid.fileIndex());
    ASSERT_FALSE(cell.isEmpty());

    // Wide enough to be the label's replacement rather than a token box beside
    // it. The layout pads a few pixels either side.
    EXPECT_GE(editor->width(), cell.width() - 12)
        << "editor " << editor->width() << "px in a " << cell.width() << "px cell";

    // It must start where the LABEL starts, not where the style guessed. The
    // label is two lines of the delegate's font at the bottom of the cell, above
    // the cell's own bottom margin -- so anything higher than that is over the
    // icon, which is what the style's SE_ItemViewItemText did.
    QFont label = grid.view.font();
    label.setPointSize(18);
    const int twoLines = 2 * QFontMetrics(label).height();
    constexpr int kBottomMarginSlack = 12;
    EXPECT_GE(editor->geometry().top(), cell.bottom() - twoLines - kBottomMarginSlack)
        << "editor top " << editor->geometry().top() << " in a cell ending at " << cell.bottom()
        << " with a " << twoLines << "px label -- the box is riding up over the icon";
    EXPECT_LE(editor->geometry().bottom(), cell.bottom());
}

TEST(ThumbnailRenameEditor, TheEditorUsesTheSameTypeAsTheLabelBesideIt) {
    Grid grid(192, 18);
    grid.delegate->setFontPointSize(17);
    QLineEdit *editor = grid.openEditor();
    ASSERT_NE(editor, nullptr);
    EXPECT_EQ(editor->font().pointSize(), 17);
}
