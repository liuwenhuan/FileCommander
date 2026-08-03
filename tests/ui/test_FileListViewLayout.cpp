#include <gtest/gtest.h>

#include <QApplication>
#include <QFile>
#include <QHeaderView>
#include <QImage>
#include <QScrollBar>
#include <QSignalSpy>
#include <QStandardItemModel>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QTest>

#include "FileListView.h"
#include "FileSystemModel.h"

namespace {

constexpr int kColumnCount = FileSystemModel::ColumnCount;

void applyThemeSheet(const QString &name) {
    QFile file(QStringLiteral(TTC_SOURCE_DIR "/resources/themes/") + name + QStringLiteral(".qss"));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text));
    qApp->setStyleSheet(QString::fromUtf8(file.readAll()));
    qApp->processEvents();
}

void populateModel(QStandardItemModel &model) {
    model.setColumnCount(kColumnCount);
    model.setRowCount(64);
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

int columnHeaderRight(FileListView &view) {
    QHeaderView *header = view.horizontalHeader();
    return header->viewport()->mapFrom(&view,
                                       QPoint(view.contentsRect().right() + 1, 0)).x();
}

void expectLastVisibleSectionAtContentRight(FileListView &view) {
    QHeaderView *header = view.horizontalHeader();
    const int last = lastVisibleColumn(*header);
    ASSERT_GE(last, 0);
    ASSERT_GT(header->viewport()->width(), 0);
    EXPECT_EQ(header->sectionViewportPosition(last) + header->sectionSize(last),
              columnHeaderRight(view));
}

QRect scrollbarSliderRect(QScrollBar &scrollbar) {
    QStyleOptionSlider opt;
    opt.initFrom(&scrollbar);
    opt.orientation = scrollbar.orientation();
    opt.minimum = scrollbar.minimum();
    opt.maximum = scrollbar.maximum();
    opt.sliderPosition = scrollbar.sliderPosition();
    opt.sliderValue = scrollbar.value();
    opt.pageStep = scrollbar.pageStep();
    opt.singleStep = scrollbar.singleStep();
    return scrollbar.style()->subControlRect(QStyle::CC_ScrollBar, &opt,
                                             QStyle::SC_ScrollBarSlider, &scrollbar);
}

QString rectDescription(const QRect &rect) {
    return QStringLiteral("(%1,%2 %3x%4)")
        .arg(rect.x()).arg(rect.y()).arg(rect.width()).arg(rect.height());
}

void expectStartupThemePaintsScrollbarHandle(const QString &theme, const QColor &handleColor) {
    const QString originalSheet = qApp->styleSheet();
    applyThemeSheet(theme);

    QStandardItemModel model;
    populateModel(model);
    FileListView view;
    view.setModel(&model);
    view.resize(700, 320);
    view.show();
    view.setPanelActive(true);
    qApp->processEvents();
    qApp->processEvents();

    QScrollBar *scrollbar = view.verticalScrollBar();
    ASSERT_NE(scrollbar, nullptr);
    ASSERT_TRUE(scrollbar->isVisible()) << theme.toStdString();
    ASSERT_GT(scrollbar->maximum(), scrollbar->minimum()) << theme.toStdString();

    const QRect slider = scrollbarSliderRect(*scrollbar);
    ASSERT_TRUE(slider.isValid()) << theme.toStdString();
    ASSERT_GT(slider.width(), 0) << theme.toStdString();
    ASSERT_GT(slider.height(), 0) << theme.toStdString();

    QHeaderView *header = view.horizontalHeader();
    ASSERT_NE(header, nullptr);
    const QRect sliderInView(scrollbar->mapTo(&view, slider.topLeft()), slider.size());
    EXPECT_GE(sliderInView.top(), header->geometry().bottom() + 1)
        << theme.toStdString() << " scrollbar enters the column header at startup";

    const QImage rendered = scrollbar->grab().toImage();
    const QPoint handlePoint = slider.center();
    ASSERT_TRUE(rendered.rect().contains(handlePoint)) << theme.toStdString();

    const QColor pixel = rendered.pixelColor(handlePoint);
    EXPECT_LE(qAbs(pixel.red() - handleColor.red()) +
                  qAbs(pixel.green() - handleColor.green()) +
                  qAbs(pixel.blue() - handleColor.blue()),
              24)
        << theme.toStdString() << " scrollbar handle is not painted at startup";

    qApp->setStyleSheet(originalSheet);
    qApp->processEvents();
}

void expectStartupThemePaintsScrollbarHandleAfterInitialLoad(const QString &theme,
                                                             const QColor &handleColor) {
    const QString originalSheet = qApp->styleSheet();
    applyThemeSheet(theme);

    QStandardItemModel model;
    model.setColumnCount(kColumnCount);
    for (int column = 0; column < kColumnCount; ++column)
        model.setHeaderData(column, Qt::Horizontal, QStringLiteral("Column %1").arg(column));

    FileListView view;
    view.setModel(&model);
    view.resize(700, 320);
    view.show();
    view.setPanelActive(true);
    qApp->processEvents();

    populateModel(model);
    qApp->processEvents();
    qApp->processEvents();

    QScrollBar *scrollbar = view.verticalScrollBar();
    ASSERT_NE(scrollbar, nullptr);
    ASSERT_TRUE(scrollbar->isVisible()) << theme.toStdString();
    ASSERT_GT(scrollbar->maximum(), scrollbar->minimum()) << theme.toStdString();

    const QRect slider = scrollbarSliderRect(*scrollbar);
    ASSERT_TRUE(slider.isValid()) << theme.toStdString();
    ASSERT_GT(slider.width(), 0) << theme.toStdString();
    ASSERT_GT(slider.height(), 0) << theme.toStdString();

    const QImage rendered = scrollbar->grab().toImage();
    const QPoint handlePoint = slider.center();
    ASSERT_TRUE(rendered.rect().contains(handlePoint)) << theme.toStdString();

    const QColor pixel = rendered.pixelColor(handlePoint);
    EXPECT_LE(qAbs(pixel.red() - handleColor.red()) +
                  qAbs(pixel.green() - handleColor.green()) +
                  qAbs(pixel.blue() - handleColor.blue()),
              24)
        << theme.toStdString() << " scrollbar handle is not painted after initial load";

    qApp->setStyleSheet(originalSheet);
    qApp->processEvents();
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

    expectLastVisibleSectionAtContentRight(m_view);
}

TEST_F(FileListViewLayoutTest, LastVisibleSectionFillsThePanelHeaderWidth) {
    resizeAndSettle(900);

    QHeaderView *header = m_view.horizontalHeader();
    const int last = lastVisibleColumn(*header);
    ASSERT_GE(last, 0);
    const int lastRightInView = header->viewport()->mapTo(
        &m_view, QPoint(header->sectionViewportPosition(last) + header->sectionSize(last), 0)).x();
    EXPECT_EQ(lastRightInView, m_view.contentsRect().right() + 1);
}

TEST_F(FileListViewLayoutTest, HiddenColumnsStillLeaveLastVisibleSectionAtHeaderViewportRightEdge) {
    QHeaderView *header = m_view.horizontalHeader();
    header->setSectionHidden(FileSystemModel::ExtColumn, true);
    header->setSectionHidden(FileSystemModel::ModifiedColumn, true);
    header->setSectionHidden(FileSystemModel::CreatedColumn, true);
    header->setSectionHidden(FileSystemModel::PermissionsColumn, true);
    resizeAndSettle(640);

    expectLastVisibleSectionAtContentRight(m_view);
}

TEST_F(FileListViewLayoutTest, NarrowViewportKeepsLastVisibleSectionAtHeaderViewportRightEdge) {
    resizeAndSettle(80);

    expectLastVisibleSectionAtContentRight(m_view);
}

TEST_F(FileListViewLayoutTest, AlwaysOnScrollbarPinsColumnsToPanelHeaderEdge) {
    ASSERT_EQ(m_view.verticalScrollBarPolicy(), Qt::ScrollBarAlwaysOn);
    resizeAndSettle(700);

    expectLastVisibleSectionAtContentRight(m_view);
}

TEST_F(FileListViewLayoutTest, VerticalScrollbarStartsBelowHeaderAndColumnsFillTheHeaderGutter) {
    ASSERT_EQ(m_view.verticalScrollBarPolicy(), Qt::ScrollBarAlwaysOn);
    resizeAndSettle(700);
    QTest::qWait(50);

    QHeaderView *header = m_view.horizontalHeader();
    QScrollBar *scrollbar = m_view.verticalScrollBar();
    ASSERT_NE(header, nullptr);
    ASSERT_NE(scrollbar, nullptr);
    ASSERT_TRUE(scrollbar->isVisible());

    EXPECT_GE(scrollbar->mapTo(&m_view, QPoint(0, 0)).y(),
              header->geometry().bottom() + 1);

    expectLastVisibleSectionAtContentRight(m_view);
}

TEST_F(FileListViewLayoutTest, ScrollbarSliderNeverEntersTheColumnHeader) {
    resizeAndSettle(700);
    QTest::qWait(50); // Cover the deferred QAbstractScrollArea geometry pass.

    QHeaderView *header = m_view.horizontalHeader();
    QScrollBar *scrollbar = m_view.verticalScrollBar();
    ASSERT_NE(header, nullptr);
    ASSERT_NE(scrollbar, nullptr);
    ASSERT_TRUE(scrollbar->isVisible());

    const QRect sliderInScrollbar = scrollbarSliderRect(*scrollbar);
    const QRect sliderInView(scrollbar->mapTo(&m_view, sliderInScrollbar.topLeft()),
                             sliderInScrollbar.size());
    EXPECT_GE(sliderInView.top(), header->geometry().bottom() + 1);
    EXPECT_EQ(header->geometry().right(), m_view.contentsRect().right());
}

TEST_F(FileListViewLayoutTest, RuntimeThemeSwitchKeepsScrollbarBelowTheColumnHeader) {
    const QString originalSheet = qApp->styleSheet();
    applyThemeSheet(QStringLiteral("green"));
    resizeAndSettle(700);
    applyThemeSheet(QStringLiteral("light"));
    QTest::qWait(100);

    QHeaderView *header = m_view.horizontalHeader();
    QScrollBar *scrollbar = m_view.verticalScrollBar();
    ASSERT_NE(header, nullptr);
    ASSERT_NE(scrollbar, nullptr);
    ASSERT_TRUE(scrollbar->isVisible());

    const QRect sliderInScrollbar = scrollbarSliderRect(*scrollbar);
    const QRect sliderInView(scrollbar->mapTo(&m_view, sliderInScrollbar.topLeft()),
                             sliderInScrollbar.size());
    const QWidget *scrollbarParent = scrollbar->parentWidget();
    const QString geometryTrace =
        QStringLiteral("header=%1 bar=%2 mappedBarTop=%3 parent=%4 parentGeometry=%5")
            .arg(rectDescription(header->geometry()))
            .arg(rectDescription(scrollbar->geometry()))
            .arg(scrollbar->mapTo(&m_view, QPoint(0, 0)).y())
            .arg(scrollbarParent ? scrollbarParent->objectName() : QStringLiteral("<null>"))
            .arg(scrollbarParent ? rectDescription(scrollbarParent->geometry())
                                 : QStringLiteral("<null>"));
    SCOPED_TRACE(geometryTrace.toStdString());
    EXPECT_GE(scrollbar->mapTo(&m_view, QPoint(0, 0)).y(),
              header->geometry().bottom() + 1);
    EXPECT_GE(sliderInView.top(), header->geometry().bottom() + 1);

    qApp->setStyleSheet(originalSheet);
    qApp->processEvents();
}

TEST_F(FileListViewLayoutTest, HeaderCoversScrollbarGutterAboveTheListBody) {
    const QString originalSheet = qApp->styleSheet();
    applyThemeSheet(QStringLiteral("green"));
    resizeAndSettle(700);
    qApp->processEvents();

    QHeaderView *header = m_view.horizontalHeader();
    QScrollBar *scrollbar = m_view.verticalScrollBar();
    ASSERT_NE(header, nullptr);
    ASSERT_NE(scrollbar, nullptr);
    ASSERT_TRUE(scrollbar->isVisible());

    const QImage rendered = m_view.grab().toImage();
    const QPoint headerGutterPoint(scrollbar->geometry().center().x(), header->geometry().center().y());
    ASSERT_TRUE(rendered.rect().contains(headerGutterPoint));

    const QColor pixel = rendered.pixelColor(headerGutterPoint);
    const QColor headerBackground(0x0a, 0x1a, 0x0d);
    EXPECT_LE(qAbs(pixel.red() - headerBackground.red()) +
                  qAbs(pixel.green() - headerBackground.green()) +
                  qAbs(pixel.blue() - headerBackground.blue()),
              12)
        << "the scrollbar gutter is visible above the column header";

    qApp->setStyleSheet(originalSheet);
}

TEST(FileListViewLayoutStartupThemeTest, DarkThemePaintsVerticalScrollbarHandleOnFirstShow) {
    expectStartupThemePaintsScrollbarHandle(QStringLiteral("dark"), QColor(0x4a, 0x4a, 0x4a));
}

TEST(FileListViewLayoutStartupThemeTest, LightThemePaintsVerticalScrollbarHandleOnFirstShow) {
    expectStartupThemePaintsScrollbarHandle(QStringLiteral("light"), QColor(0xc0, 0xc0, 0xc0));
}

TEST(FileListViewLayoutStartupThemeTest, GreenThemePaintsVerticalScrollbarHandleOnFirstShow) {
    expectStartupThemePaintsScrollbarHandle(QStringLiteral("green"), QColor(0x12, 0x60, 0x2f));
}

TEST(FileListViewLayoutStartupThemeTest, GreenThemePaintsVerticalScrollbarHandleAfterInitialLoad) {
    expectStartupThemePaintsScrollbarHandleAfterInitialLoad(QStringLiteral("green"),
                                                            QColor(0x12, 0x60, 0x2f));
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
    const QVector<int> baseAfter = m_view.columnBaseWidths();
    SCOPED_TRACE(QStringLiteral("handles=[%1] resizes=[%2] baseExt=%3 baseSize=%4")
                     .arg(handleEvents.join(QLatin1String(", ")),
                          resizeEvents.join(QLatin1String(", ")))
                     .arg(baseAfter.value(FileSystemModel::ExtColumn))
                     .arg(baseAfter.value(FileSystemModel::SizeColumn))
                     .toStdString());
    EXPECT_EQ(handleSpy.first().first().toInt(), FileSystemModel::ExtColumn);
    EXPECT_GE(m_view.columnWidth(FileSystemModel::SizeColumn),
              m_view.fontMetrics().horizontalAdvance(widest) + 24);
    EXPECT_EQ(m_view.columnWidth(FileSystemModel::ExtColumn), extBefore);
    expectLastVisibleSectionAtContentRight(m_view);
}

// Space reports the row so the panel can count a directory's size. Total
// Commander only does this for an *unselected* directory, which works there
// because clicking never selects; this view is ExtendedSelection, so the row
// under the cursor is usually already selected -- including after a Ctrl+click
// or Shift+click multi-select -- and carrying TC's rule across would make the
// count almost never happen. It must fire either way.
TEST_F(FileListViewLayoutTest, SpaceReportsTheRowWhetherOrNotItIsAlreadySelected) {
    QSignalSpy spy(&m_view, &FileListView::rowSpaced);
    const QModelIndex row3 = m_model.index(3, 0);

    // Unselected row under the cursor. NoUpdate because the plain
    // setCurrentIndex() would select it as a side effect (this view is
    // ExtendedSelection), which is the very state this half must not be in.
    m_view.selectionModel()->setCurrentIndex(row3, QItemSelectionModel::NoUpdate);
    m_view.selectionModel()->clearSelection();
    ASSERT_FALSE(m_view.selectionModel()->isSelected(row3));
    QTest::keyClick(&m_view, Qt::Key_Space);
    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.takeFirst().at(0).toInt(), 3);

    // Already selected, as a Ctrl+click or Shift+click multi-select leaves it.
    const QModelIndex row5 = m_model.index(5, 0);
    m_view.selectionModel()->select(m_model.index(4, 0),
                                    QItemSelectionModel::Select | QItemSelectionModel::Rows);
    m_view.selectionModel()->select(row5,
                                    QItemSelectionModel::Select | QItemSelectionModel::Rows);
    m_view.selectionModel()->setCurrentIndex(row5, QItemSelectionModel::NoUpdate);
    ASSERT_TRUE(m_view.selectionModel()->isSelected(row5));
    QTest::keyClick(&m_view, Qt::Key_Space);
    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.takeFirst().at(0).toInt(), 5);
}

// Arrow keys move the cursor without touching the selection -- that is what
// gives Space something to switch on. The consequence is that the cursor is the
// ONLY thing that moves, so something has to paint it: with the selection
// highlight left behind and nothing drawn under the cursor, arrowing through the
// list looks like a dead key. Assert both halves, movement and paint.
TEST_F(FileListViewLayoutTest, ArrowKeysMoveTheCursorAndTheCursorRowIsPainted) {
    resizeAndSettle(900);
    m_view.selectionModel()->setCurrentIndex(m_model.index(2, 0),
                                             QItemSelectionModel::NoUpdate);
    m_view.selectionModel()->clearSelection();

    QTest::keyClick(&m_view, Qt::Key_Down);

    EXPECT_EQ(m_view.currentIndex().row(), 3);
    EXPECT_TRUE(m_view.selectionModel()->selectedRows().isEmpty());

    qApp->processEvents();
    const QRect cursorRow = m_view.visualRect(m_model.index(3, 0));
    const QRect plainRow = m_view.visualRect(m_model.index(5, 0));
    ASSERT_FALSE(cursorRow.isEmpty());
    ASSERT_FALSE(plainRow.isEmpty());

    QImage shot(m_view.viewport()->size(), QImage::Format_ARGB32);
    shot.fill(Qt::transparent);
    m_view.viewport()->render(&shot);

    // The two rows carry different text, so compare only the left margin strip,
    // which no glyph reaches -- any difference there is the cursor indicator.
    auto marginStrip = [&shot](const QRect &row) {
        QVector<QRgb> strip;
        for (int y = row.top(); y <= row.bottom(); ++y)
            strip << shot.pixel(row.left() + 1, y);
        return strip;
    };
    EXPECT_NE(marginStrip(cursorRow), marginStrip(plainRow))
        << "nothing distinguishes the row under the cursor from an ordinary row";
}

// Insert is the plain selection key: it must NOT trigger the size count, or
// every Insert on a directory would kick off a recursive scan.
TEST_F(FileListViewLayoutTest, InsertTogglesSelectionWithoutReportingTheRow) {
    QSignalSpy spy(&m_view, &FileListView::rowSpaced);
    const QModelIndex row2 = m_model.index(2, 0);
    m_view.selectionModel()->setCurrentIndex(row2, QItemSelectionModel::NoUpdate);
    m_view.selectionModel()->clearSelection();

    QTest::keyClick(&m_view, Qt::Key_Insert);

    EXPECT_EQ(spy.count(), 0);
    EXPECT_TRUE(m_view.selectionModel()->isSelected(row2));      // still selects
    EXPECT_EQ(m_view.currentIndex().row(), 3);                   // and steps down
}

} // namespace
