#include <gtest/gtest.h>

#include <QApplication>
#include <QHeaderView>
#include <QStandardItemModel>

#include "FileListView.h"
#include "FileSystemModel.h"

namespace {

constexpr int kColumnCount = FileSystemModel::ColumnCount;

void populateModel(QStandardItemModel &model) {
    model.setColumnCount(kColumnCount);
    for (int column = 0; column < kColumnCount; ++column)
        model.setHeaderData(column, Qt::Horizontal, QStringLiteral("Column %1").arg(column));
    for (int row = 0; row < 64; ++row)
        for (int column = 0; column < kColumnCount; ++column)
            model.setData(model.index(row, column), QStringLiteral("value-%1-%2").arg(row).arg(column));
}

int lastVisibleColumn(const QHeaderView &header) {
    for (int column = header.count() - 1; column >= 0; --column)
        if (!header.isSectionHidden(column))
            return column;
    return -1;
}

void expectLastVisibleSectionAtHeaderViewportRight(FileListView &view) {
    QHeaderView *header = view.horizontalHeader();
    const int last = lastVisibleColumn(*header);
    ASSERT_GE(last, 0);
    ASSERT_GT(header->viewport()->width(), 0);
    EXPECT_EQ(header->sectionViewportPosition(last) + header->sectionSize(last),
              header->viewport()->width());
}

class FileListViewLayoutTest : public ::testing::Test {
protected:
    void SetUp() override {
        populateModel(m_model);
        m_view.setModel(&m_model);
        m_view.show();
    }

    void resizeAndSettle(int width) {
        m_view.resize(width, 320);
        m_view.show();
        qApp->processEvents();
    }

    QStandardItemModel m_model;
    FileListView m_view;
};

TEST_F(FileListViewLayoutTest, LastVisibleSectionReachesHeaderViewportRightEdge) {
    resizeAndSettle(900);

    expectLastVisibleSectionAtHeaderViewportRight(m_view);
}

TEST_F(FileListViewLayoutTest, HiddenColumnsStillLeaveLastVisibleSectionAtHeaderViewportRightEdge) {
    QHeaderView *header = m_view.horizontalHeader();
    header->setSectionHidden(FileSystemModel::ExtColumn, true);
    header->setSectionHidden(FileSystemModel::ModifiedColumn, true);
    header->setSectionHidden(FileSystemModel::CreatedColumn, true);
    header->setSectionHidden(FileSystemModel::PermissionsColumn, true);
    resizeAndSettle(640);

    expectLastVisibleSectionAtHeaderViewportRight(m_view);
}

TEST_F(FileListViewLayoutTest, NarrowViewportKeepsLastVisibleSectionAtHeaderViewportRightEdge) {
    resizeAndSettle(80);

    expectLastVisibleSectionAtHeaderViewportRight(m_view);
}

TEST_F(FileListViewLayoutTest, AlwaysOnScrollbarDoesNotReserveExtraHeaderWidth) {
    ASSERT_EQ(m_view.verticalScrollBarPolicy(), Qt::ScrollBarAlwaysOn);
    resizeAndSettle(700);

    expectLastVisibleSectionAtHeaderViewportRight(m_view);
}

} // namespace
