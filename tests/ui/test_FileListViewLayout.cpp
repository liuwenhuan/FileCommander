#include <gtest/gtest.h>

#include <QApplication>
#include <QHeaderView>
#include <QSignalSpy>
#include <QStandardItemModel>
#include <QTest>

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

TEST_F(FileListViewLayoutTest, DoubleClickingDividerAutoFitsTheColumnOnItsRight) {
    resizeAndSettle(1000);
    const QString widest = QStringLiteral("119.5 GB");
    m_model.setData(m_model.index(0, FileSystemModel::SizeColumn), widest);
    m_model.setData(m_model.index(0, FileSystemModel::ExtColumn), QStringLiteral("txt"));

    QVector<int> widths = m_view.columnBaseWidths();
    ASSERT_GT(widths.size(), FileSystemModel::SizeColumn);
    widths[FileSystemModel::ExtColumn] = 180;
    widths[FileSystemModel::SizeColumn] = 40;
    m_view.restoreColumnLayout(widths, -1, -1, Qt::AscendingOrder);
    qApp->processEvents();
    ASSERT_LT(m_view.columnWidth(FileSystemModel::SizeColumn),
              m_view.fontMetrics().horizontalAdvance(widest));
    const int extBefore = m_view.columnWidth(FileSystemModel::ExtColumn);
    ASSERT_EQ(m_view.columnBaseWidths().at(FileSystemModel::ExtColumn), extBefore);

    QHeaderView *header = m_view.horizontalHeader();
    QSignalSpy handleSpy(header, &QHeaderView::sectionHandleDoubleClicked);
    QSignalSpy resizeSpy(header, &QHeaderView::sectionResized);
    const int divider = header->sectionViewportPosition(FileSystemModel::ExtColumn) +
                        header->sectionSize(FileSystemModel::ExtColumn);
    QTest::mouseDClick(header->viewport(), Qt::LeftButton, Qt::NoModifier,
                       QPoint(divider, header->height() / 2));
    qApp->processEvents();

    ASSERT_FALSE(handleSpy.isEmpty()) << "the double-click did not hit a header resize handle";
    QStringList resizeEvents;
    for (const auto &event : resizeSpy)
        resizeEvents << QStringLiteral("%1:%2->%3")
                            .arg(event.at(0).toInt())
                            .arg(event.at(1).toInt())
                            .arg(event.at(2).toInt());
    QStringList handleEvents;
    for (const auto &event : handleSpy)
        handleEvents << QString::number(event.at(0).toInt());
    SCOPED_TRACE(QStringLiteral("handles=[%1] resizes=[%2]")
                     .arg(handleEvents.join(QLatin1String(", ")),
                          resizeEvents.join(QLatin1String(", ")))
                     .toStdString());
    EXPECT_EQ(handleSpy.first().first().toInt(), FileSystemModel::ExtColumn);
    EXPECT_GE(m_view.columnWidth(FileSystemModel::SizeColumn),
              m_view.fontMetrics().horizontalAdvance(widest) + 24);
    EXPECT_EQ(m_view.columnWidth(FileSystemModel::ExtColumn), extBefore);
}

} // namespace
