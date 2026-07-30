#include <gtest/gtest.h>

#include <QAbstractAnimation>
#include <QApplication>
#include <QColor>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <QVariantAnimation>

#include "FileListView.h"
#include "FileSystemModel.h"
#include "IconFileView.h"
#include "MotionPolicy.h"

namespace {

class MotionPolicyStateGuard {
public:
    MotionPolicyStateGuard() {
        MotionPolicy::clearReducedForTest();
        MotionPolicy::clearSystemReducedForTest();
        MotionPolicy::setApplicationReduced(false);
    }

    ~MotionPolicyStateGuard() {
        MotionPolicy::clearReducedForTest();
        MotionPolicy::clearSystemReducedForTest();
        MotionPolicy::setApplicationReduced(false);
    }
};

class DragMotionListView : public FileListView {
public:
    using FileListView::dragEnterEvent;
    using FileListView::dragLeaveEvent;
    using FileListView::dragMoveEvent;
    using FileListView::dropEvent;
};

class DragMotionIconView : public IconFileView {
public:
    using IconFileView::dragEnterEvent;
    using IconFileView::dragLeaveEvent;
    using IconFileView::dragMoveEvent;
    using IconFileView::dropEvent;
};

template <typename View>
struct Panel {
    QTemporaryDir dir;
    FileSystemModel model;
    View view;

    Panel() {
        model.setRootPath(dir.path());
        view.setModel(&model);
        view.resize(400, 300);
        view.show();
        qApp->processEvents();
    }
};

struct Geometry {
    QRect view;
    QRect viewport;
    QSize sizeHint;
    QSize minimumSizeHint;
    int verticalScroll;
    int horizontalScroll;
};

template <typename View>
Geometry geometryOf(const View &view) {
    return {view.geometry(), view.viewport()->geometry(), view.sizeHint(), view.minimumSizeHint(),
            view.verticalScrollBar()->value(), view.horizontalScrollBar()->value()};
}

template <typename View>
void expectGeometry(const View &view, const Geometry &before) {
    EXPECT_EQ(view.geometry(), before.view);
    EXPECT_EQ(view.viewport()->geometry(), before.viewport);
    EXPECT_EQ(view.sizeHint(), before.sizeHint);
    EXPECT_EQ(view.minimumSizeHint(), before.minimumSizeHint);
    EXPECT_EQ(view.verticalScrollBar()->value(), before.verticalScroll);
    EXPECT_EQ(view.horizontalScrollBar()->value(), before.horizontalScroll);
}

void setValidMime(QMimeData *mime) {
    mime->setUrls({QUrl::fromLocalFile(QStringLiteral("C:/drag-source.txt"))});
}

template <typename View>
QVariantAnimation *feedbackAnimation(View &view) {
    return view.findChild<QVariantAnimation *>(QStringLiteral("DragTargetFeedbackAnimation"));
}

template <typename View>
void sendValidEnterAndMove(View &view, QMimeData *mime) {
    QDragEnterEvent enter(QPoint(8, 8), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
    view.dragEnterEvent(&enter);
    EXPECT_TRUE(enter.isAccepted());

    QDragMoveEvent move(QPoint(12, 12), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
    view.dragMoveEvent(&move);
    EXPECT_TRUE(move.isAccepted());
}

template <typename View>
void sendDrop(View &view, QMimeData *mime) {
    QDropEvent drop(QPoint(12, 12), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
    view.dropEvent(&drop);
    EXPECT_TRUE(drop.isAccepted());
}

template <typename View>
void acceptedDropUsesPaintOnlySuccessFeedback() {
    MotionPolicyStateGuard guard;
    MotionPolicy::setReducedForTest(false);

    Panel<View> panel;
    const Geometry before = geometryOf(panel.view);
    QMimeData mime;
    setValidMime(&mime);
    sendValidEnterAndMove(panel.view, &mime);

    EXPECT_EQ(panel.view.property("dragFeedbackState").toString(), QStringLiteral("accepted"));
    QVariantAnimation *animation = feedbackAnimation(panel.view);
    ASSERT_NE(animation, nullptr);
    EXPECT_EQ(animation->duration(), 80);
    EXPECT_EQ(animation->state(), QAbstractAnimation::Running);

    sendDrop(panel.view, &mime);

    EXPECT_EQ(panel.view.property("dragFeedbackState").toString(), QStringLiteral("success"));
    EXPECT_EQ(animation->duration(), MotionPolicy::duration(MotionDuration::Normal));
    EXPECT_EQ(animation->state(), QAbstractAnimation::Running);
    expectGeometry(panel.view, before);

    QTRY_COMPARE_WITH_TIMEOUT(panel.view.property("dragFeedbackState").toString(),
                              QStringLiteral("none"), 300);
    expectGeometry(panel.view, before);
}

template <typename View>
void rejectedLeaveAndModelResetClearFeedback() {
    MotionPolicyStateGuard guard;
    MotionPolicy::setReducedForTest(false);

    Panel<View> panel;
    const Geometry before = geometryOf(panel.view);
    QMimeData rejected;
    rejected.setText(QStringLiteral("not a file payload"));

    QDragEnterEvent enter(QPoint(8, 8), Qt::CopyAction, &rejected, Qt::LeftButton, Qt::NoModifier);
    panel.view.dragEnterEvent(&enter);
    EXPECT_FALSE(enter.isAccepted());
    EXPECT_EQ(panel.view.property("dragFeedbackState").toString(), QStringLiteral("rejected"));

    QVariantAnimation *animation = feedbackAnimation(panel.view);
    ASSERT_NE(animation, nullptr);
    EXPECT_EQ(animation->duration(), 80);
    EXPECT_NE(panel.view.property("dragFeedbackColor").value<QColor>(),
              panel.view.palette().color(QPalette::Highlight));

    QDragLeaveEvent leave;
    panel.view.dragLeaveEvent(&leave);
    EXPECT_TRUE(leave.isAccepted());
    EXPECT_EQ(panel.view.property("dragFeedbackState").toString(), QStringLiteral("none"));
    EXPECT_NE(animation->state(), QAbstractAnimation::Running);

    QMimeData accepted;
    setValidMime(&accepted);
    sendValidEnterAndMove(panel.view, &accepted);
    ASSERT_EQ(panel.view.property("dragFeedbackState").toString(), QStringLiteral("accepted"));
    panel.model.setNameFilter(QStringLiteral("reset feedback"));
    EXPECT_EQ(panel.view.property("dragFeedbackState").toString(), QStringLiteral("none"));
    expectGeometry(panel.view, before);
}

template <typename View>
void reducedMotionShowsStaticFinalFeedback() {
    MotionPolicyStateGuard guard;
    MotionPolicy::setReducedForTest(true);

    Panel<View> panel;
    QMimeData mime;
    setValidMime(&mime);
    sendValidEnterAndMove(panel.view, &mime);
    QVariantAnimation *animation = feedbackAnimation(panel.view);
    ASSERT_NE(animation, nullptr);
    EXPECT_EQ(panel.view.property("dragFeedbackState").toString(), QStringLiteral("accepted"));
    EXPECT_NE(animation->state(), QAbstractAnimation::Running);

    sendDrop(panel.view, &mime);
    EXPECT_EQ(panel.view.property("dragFeedbackState").toString(), QStringLiteral("success"));
    EXPECT_NE(animation->state(), QAbstractAnimation::Running);
}

} // namespace

TEST(DragMotion, FileListAcceptedTargetAndDropArePaintOnly) {
    acceptedDropUsesPaintOnlySuccessFeedback<DragMotionListView>();
}

TEST(DragMotion, IconViewAcceptedTargetAndDropArePaintOnly) {
    acceptedDropUsesPaintOnlySuccessFeedback<DragMotionIconView>();
}

TEST(DragMotion, FileListRejectedLeaveAndModelResetClearFeedback) {
    rejectedLeaveAndModelResetClearFeedback<DragMotionListView>();
}

TEST(DragMotion, IconViewRejectedLeaveAndModelResetClearFeedback) {
    rejectedLeaveAndModelResetClearFeedback<DragMotionIconView>();
}

TEST(DragMotion, FileListReducedMotionShowsStaticFinalFeedback) {
    reducedMotionShowsStaticFinalFeedback<DragMotionListView>();
}

TEST(DragMotion, IconViewReducedMotionShowsStaticFinalFeedback) {
    reducedMotionShowsStaticFinalFeedback<DragMotionIconView>();
}
