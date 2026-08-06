#include <gtest/gtest.h>

#include "TryUntil.h"

#include <QAbstractAnimation>
#include <QApplication>
#include <QColor>
#include <QDir>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QFile>
#include <QMimeData>
#include <QPointer>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <QVariantAnimation>

#include <memory>

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
    }

    ~MotionPolicyStateGuard() {
        MotionPolicy::clearReducedForTest();
        MotionPolicy::clearSystemReducedForTest();
    }
};

class ApplicationStyleSheetGuard {
public:
    ApplicationStyleSheetGuard() : m_original(qApp->styleSheet()) {}

    ~ApplicationStyleSheetGuard() {
        qApp->setStyleSheet(m_original);
        qApp->processEvents();
    }

private:
    QString m_original;
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
    return view.template findChild<QVariantAnimation *>(
        QStringLiteral("DragTargetFeedbackAnimation"));
}

template <typename View>
QList<QVariantAnimation *> feedbackAnimations(View &view) {
    return view.template findChildren<QVariantAnimation *>(
        QStringLiteral("DragTargetFeedbackAnimation"));
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

    FC_TRY_COMPARE_WITH_TIMEOUT(panel.view.property("dragFeedbackState").toString(),
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
    EXPECT_NE(panel.view.property("dragFeedbackColor").template value<QColor>(),
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

template <typename View>
void directCancelAndDestructionClearActiveFeedback() {
    MotionPolicyStateGuard guard;
    MotionPolicy::setReducedForTest(false);

    {
        Panel<View> panel;
        QMimeData mime;
        setValidMime(&mime);
        sendValidEnterAndMove(panel.view, &mime);

        QVariantAnimation *animation = feedbackAnimation(panel.view);
        ASSERT_NE(animation, nullptr);
        ASSERT_EQ(animation->state(), QAbstractAnimation::Running);

        QDragLeaveEvent cancel;
        panel.view.dragLeaveEvent(&cancel);
        EXPECT_TRUE(cancel.isAccepted());
        EXPECT_EQ(panel.view.property("dragFeedbackState").toString(), QStringLiteral("none"));
        EXPECT_FALSE(panel.view.property("dragFeedbackColor").template value<QColor>().isValid());
        EXPECT_NE(animation->state(), QAbstractAnimation::Running);

        QTest::qWait(MotionPolicy::duration(MotionDuration::Normal) + 100);
        EXPECT_EQ(panel.view.property("dragFeedbackState").toString(), QStringLiteral("none"));
        EXPECT_FALSE(panel.view.property("dragFeedbackColor").template value<QColor>().isValid());
        EXPECT_NE(animation->state(), QAbstractAnimation::Running);
    }

    {
        auto panel = std::make_unique<Panel<View>>();
        QMimeData mime;
        setValidMime(&mime);
        sendValidEnterAndMove(panel->view, &mime);
        ASSERT_EQ(feedbackAnimation(panel->view)->state(), QAbstractAnimation::Running);

        QPointer<View> watched = &panel->view;
        panel.reset();
        EXPECT_TRUE(watched.isNull());
        QTest::qWait(MotionPolicy::duration(MotionDuration::Normal) + 100);
    }

    MotionPolicy::setReducedForTest(true);
    {
        auto panel = std::make_unique<Panel<View>>();
        QMimeData mime;
        setValidMime(&mime);
        sendValidEnterAndMove(panel->view, &mime);
        sendDrop(panel->view, &mime);
        ASSERT_EQ(panel->view.property("dragFeedbackState").toString(),
                  QStringLiteral("success"));
        ASSERT_NE(feedbackAnimation(panel->view)->state(), QAbstractAnimation::Running);

        QPointer<View> watched = &panel->view;
        panel.reset();
        EXPECT_TRUE(watched.isNull());
        QTest::qWait(250);
    }
}

template <typename View>
void burstDragMovesReuseOneAnimationWithoutRestarting() {
    MotionPolicyStateGuard guard;
    MotionPolicy::setReducedForTest(false);

    Panel<View> panel;
    QMimeData mime;
    setValidMime(&mime);
    QDragEnterEvent enter(QPoint(8, 8), Qt::CopyAction, &mime, Qt::LeftButton,
                          Qt::NoModifier);
    panel.view.dragEnterEvent(&enter);
    ASSERT_TRUE(enter.isAccepted());

    QList<QVariantAnimation *> animations = feedbackAnimations(panel.view);
    ASSERT_EQ(animations.size(), 1);
    QVariantAnimation *animation = animations.first();
    ASSERT_EQ(animation->state(), QAbstractAnimation::Running);
    // A generous budget for "the animation has made some progress", because
    // that is all this waits for -- the burst below is what the test is about.
    // At 100 ms it failed whenever the machine was busy enough to starve the
    // animation, and before these waits could fail at all it simply left the
    // test early and reported a pass, so the burst was never exercised there.
    FC_TRY_VERIFY_WITH_TIMEOUT(animation->currentTime() >= 20, 2000);
    const int timeBeforeBurst = animation->currentTime();

    for (int i = 0; i < 8; ++i) {
        QTest::qWait(5);
        QDragMoveEvent move(QPoint(12 + i, 12), Qt::CopyAction, &mime, Qt::LeftButton,
                            Qt::NoModifier);
        panel.view.dragMoveEvent(&move);
        EXPECT_TRUE(move.isAccepted());
    }

    EXPECT_EQ(feedbackAnimations(panel.view).size(), 1);
    EXPECT_EQ(feedbackAnimation(panel.view), animation);
    EXPECT_GE(animation->currentTime(), timeBeforeBurst);
    FC_TRY_COMPARE_WITH_TIMEOUT(animation->state(), QAbstractAnimation::Stopped, 2000);
    EXPECT_EQ(panel.view.property("dragFeedbackState").toString(), QStringLiteral("accepted"));
}

QByteArray readTheme(const QString &name) {
    QFile file(QDir(QStringLiteral(TTC_SOURCE_DIR))
                   .filePath(QStringLiteral("resources/themes/%1.qss").arg(name)));
    EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << qPrintable(file.fileName());
    return file.readAll();
}

template <typename View>
void expectThemeFeedback(const QString &theme, const QColor &highlight, const QColor &text) {
    SCOPED_TRACE(theme.toStdString());
    qApp->setStyleSheet(QString::fromUtf8(readTheme(theme)));
    qApp->processEvents();

    Panel<View> panel;
    EXPECT_EQ(panel.view.palette().color(QPalette::Highlight), highlight);
    EXPECT_EQ(panel.view.palette().color(QPalette::Text), text);

    QColor acceptedFinal = highlight;
    acceptedFinal.setAlpha(210);
    QMimeData accepted;
    setValidMime(&accepted);
    QDragEnterEvent acceptedEnter(QPoint(8, 8), Qt::CopyAction, &accepted, Qt::LeftButton,
                                  Qt::NoModifier);
    panel.view.dragEnterEvent(&acceptedEnter);
    ASSERT_TRUE(acceptedEnter.isAccepted());
    FC_TRY_COMPARE_WITH_TIMEOUT(panel.view.property("dragFeedbackColor").template value<QColor>(),
                              acceptedFinal, 150);

    QDragLeaveEvent leave;
    panel.view.dragLeaveEvent(&leave);

    QColor rejectedFinal = text;
    rejectedFinal.setAlpha(180);
    QMimeData rejected;
    rejected.setText(QStringLiteral("not a file payload"));
    QDragEnterEvent rejectedEnter(QPoint(8, 8), Qt::CopyAction, &rejected, Qt::LeftButton,
                                  Qt::NoModifier);
    panel.view.dragEnterEvent(&rejectedEnter);
    ASSERT_FALSE(rejectedEnter.isAccepted());

    for (int elapsed = 0; elapsed <= 100; elapsed += 10) {
        EXPECT_NE(panel.view.property("dragFeedbackColor").template value<QColor>(), acceptedFinal);
        QTest::qWait(10);
    }
    EXPECT_EQ(panel.view.property("dragFeedbackColor").template value<QColor>(), rejectedFinal);
}

template <typename View>
void themeStylesheetsDriveFinalFeedbackColors() {
    MotionPolicyStateGuard motionGuard;
    ApplicationStyleSheetGuard styleGuard;
    MotionPolicy::setReducedForTest(false);

    // The dark theme's accent is a neutral slate, not a blue: its file grid is
    // greyscale once the icons follow the theme, and a saturated accent was the
    // only coloured thing on screen. See the note atop dark.qss.
    expectThemeFeedback<View>(QStringLiteral("dark"), QColor(QStringLiteral("#565b63")),
                              QColor(QStringLiteral("#e0e0e0")));
    expectThemeFeedback<View>(QStringLiteral("light"), QColor(QStringLiteral("#3d7deb")),
                              QColor(QStringLiteral("#202020")));
    expectThemeFeedback<View>(QStringLiteral("green"), QColor(QStringLiteral("#33ff88")),
                              QColor(QStringLiteral("#33ff88")));
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

TEST(DragMotion, FileListDirectCancelAndDestructionClearActiveFeedback) {
    directCancelAndDestructionClearActiveFeedback<DragMotionListView>();
}

TEST(DragMotion, IconViewDirectCancelAndDestructionClearActiveFeedback) {
    directCancelAndDestructionClearActiveFeedback<DragMotionIconView>();
}

TEST(DragMotion, FileListBurstDragMovesReuseOneAnimationWithoutRestarting) {
    burstDragMovesReuseOneAnimationWithoutRestarting<DragMotionListView>();
}

TEST(DragMotion, IconViewBurstDragMovesReuseOneAnimationWithoutRestarting) {
    burstDragMovesReuseOneAnimationWithoutRestarting<DragMotionIconView>();
}

TEST(DragMotion, FileListThemeStylesheetsDriveFinalFeedbackColors) {
    themeStylesheetsDriveFinalFeedbackColors<DragMotionListView>();
}

TEST(DragMotion, IconViewThemeStylesheetsDriveFinalFeedbackColors) {
    themeStylesheetsDriveFinalFeedbackColors<DragMotionIconView>();
}
