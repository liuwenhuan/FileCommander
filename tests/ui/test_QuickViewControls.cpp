#include <gtest/gtest.h>

#include <QAction>
#include <QCheckBox>
#include <QDir>
#include <QSlider>
#include <QTemporaryDir>
#include <QTest>
#include <QStringList>
#include <QPointer>
#include <QToolBar>

#include <memory>

#include <QFile>
#include <QImage>
#include <QPainter>

#include "QuickView.h"
#include "media/WindowsMediaSurface.h"
#include "Settings.h"
#include "media/MediaEngine.h"

// The video transport shares one row with a seek bar, and a narrow preview pane
// used to take every pixel out of the seek bar because the volume slider was
// fixed-width. The seek bar is the control that has to survive: a 20px one
// cannot be seeked with.
namespace {

QSlider *sliderNamed(const QWidget &view, const QString &objectName) {
    return view.findChild<QSlider *>(objectName);
}

QStringList toolbarActionTexts(const QWidget &view) {
    QStringList texts;
    for (QToolBar *bar : view.findChildren<QToolBar *>())
        for (QAction *action : bar->actions())
            if (!action->text().isEmpty())
                texts << action->text();
    return texts;
}

// The video page wires itself to a media engine; this stands in for one so the
// transport's LAYOUT can be checked without a working backend.
class StubMediaEngine final : public MediaEngine {
public:
    void initialize() override {}
    void load(const MediaSource &, MediaKind) override {}
    QWidget *videoSurface() override {
        if (!m_surface)
            m_surface = new QWidget;
        return m_surface;
    }

private:
    QPointer<QWidget> m_surface;
};

} // namespace

TEST(QuickViewControls, VolumeGivesWayBeforeTheSeekBarDoes) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Settings settings(dir.filePath(QStringLiteral("settings.ini")));
    QuickView view(settings, QuickView::Context::Embedded, nullptr,
                   std::make_unique<StubMediaEngine>());
    view.resize(900, 600);
    view.show();
    qApp->processEvents();

    view.buildVideoPageForTest();
    QSlider *seek = sliderNamed(view, QStringLiteral("quickViewVideoSeek"));
    QSlider *volume = sliderNamed(view, QStringLiteral("quickViewVideoVolume"));
    ASSERT_NE(seek, nullptr);
    ASSERT_NE(volume, nullptr);

    // The volume slider must be able to shrink at all -- a fixed width is what
    // forced the whole shortfall onto its neighbour.
    EXPECT_LT(volume->minimumWidth(), volume->maximumWidth())
        << "volume cannot give up any width, so the seek bar pays for every "
           "pixel the pane loses";

    if (seek) {
        EXPECT_GT(seek->minimumWidth(), volume->minimumWidth())
            << "the seek bar must keep more room than volume, not less";
        EXPECT_GE(seek->minimumWidth(), 120) << "a seek bar this short cannot be seeked with";
    }
}

// "Show info" is a view option, like the image page's, and belongs on a toolbar
// rather than among the transport controls -- which is also the row that runs
// out of width first.
TEST(QuickViewControls, TheVideoPageHasAToolbarCarryingShowInfo) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Settings settings(dir.filePath(QStringLiteral("settings.ini")));
    QuickView view(settings, QuickView::Context::Embedded, nullptr,
                   std::make_unique<StubMediaEngine>());
    view.show();
    qApp->processEvents();

    QWidget *videoPage = view.buildVideoPageForTest();
    ASSERT_NE(videoPage, nullptr);

    // Specifically the VIDEO page's own toolbar -- counting every toolbar in
    // the widget would be satisfied by the image page's alone, which is how the
    // first draft of this test passed before the toolbar existed.
    QStringList onToolbar;
    for (QToolBar *bar : videoPage->findChildren<QToolBar *>())
        for (QCheckBox *box : bar->findChildren<QCheckBox *>())
            onToolbar << box->text();
    EXPECT_FALSE(onToolbar.isEmpty()) << "the video page has no toolbar of its own";

    // ...and it is not still down among the transport controls.
    QStringList offToolbar;
    for (QCheckBox *box : videoPage->findChildren<QCheckBox *>())
        if (!qobject_cast<QToolBar *>(box->parentWidget()))
            offToolbar << box->text();
    EXPECT_TRUE(offToolbar.isEmpty())
        << "still on the transport row: " << offToolbar.join(QStringLiteral(", ")).toStdString();
}

// Prev/Next came off the image toolbar. The underlying navigation stays -- the
// audio page uses it for tracks, where "next" means something.
TEST(QuickViewControls, TheImageToolbarNoLongerOffersPrevAndNext) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Settings settings(dir.filePath(QStringLiteral("settings.ini")));
    QuickView view(settings);
    view.show();
    qApp->processEvents();

    const QStringList texts = toolbarActionTexts(view);
    ASSERT_FALSE(texts.isEmpty()) << "no toolbar actions found at all";
    EXPECT_FALSE(texts.contains(QStringLiteral("< Prev"))) << texts.join(QStringLiteral(", ")).toStdString();
    EXPECT_FALSE(texts.contains(QStringLiteral("Next >"))) << texts.join(QStringLiteral(", ")).toStdString();
    // ...and the rest of the toolbar is still there.
    EXPECT_TRUE(texts.contains(QStringLiteral("Fit")));
}

// Rotation is applied when the frame is PAINTED, not to the decoded frame:
// rotating every frame would cost a full-resolution transform per frame for
// something the painter does on the way to screen anyway. So it is checked by
// looking at what lands on the widget.
TEST(QuickViewControls, TheVideoSurfacePaintsAQuarterTurn) {
    WindowsMediaSurface surface;
    surface.resize(200, 200);

    // A frame that is unambiguous under rotation: red on the left half, blue on
    // the right. Wider than tall, so a quarter turn also swaps which axis the
    // fit is bound by.
    QImage frame(80, 40, QImage::Format_ARGB32);
    frame.fill(Qt::blue);
    QPainter painter(&frame);
    painter.fillRect(QRect(0, 0, 40, 40), Qt::red);
    painter.end();
    surface.setFrame(frame);

    auto colourAt = [&surface](double xFraction, double yFraction) {
        const QImage shot = surface.grab().toImage();
        return shot.pixelColor(int(shot.width() * xFraction), int(shot.height() * yFraction));
    };

    ASSERT_EQ(surface.rotation(), 0);
    EXPECT_EQ(colourAt(0.25, 0.5), QColor(Qt::red)) << "unrotated: red is on the left";

    // A quarter turn clockwise puts the red half at the TOP.
    surface.setRotation(90);
    EXPECT_EQ(surface.rotation(), 90);
    EXPECT_EQ(colourAt(0.5, 0.25), QColor(Qt::red)) << "90 degrees: red should be on top";
    EXPECT_EQ(colourAt(0.5, 0.75), QColor(Qt::blue));

    // Normalisation: -90 and 270 are the same turn.
    surface.setRotation(-90);
    EXPECT_EQ(surface.rotation(), 270);
    EXPECT_EQ(colourAt(0.5, 0.75), QColor(Qt::red)) << "270 degrees: red should be at the bottom";
}

// The rotation corrected the LAST clip. Carrying it into the next one would
// silently misorient a clip that was fine.
TEST(QuickViewControls, ANewClipStartsTheRightWayUp) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Settings settings(dir.filePath(QStringLiteral("settings.ini")));
    QuickView view(settings, QuickView::Context::Embedded, nullptr,
                   std::make_unique<StubMediaEngine>());
    view.show();
    qApp->processEvents();
    QWidget *videoPage = view.buildVideoPageForTest();
    ASSERT_NE(videoPage, nullptr);

    // Inside the VIDEO page: the image toolbar has a "Rotate Right" of its own,
    // and picking that one would rotate an image and leave this assertion
    // measuring nothing.
    QAction *rotateRight = nullptr;
    for (QToolBar *bar : videoPage->findChildren<QToolBar *>())
        for (QAction *action : bar->actions())
            if (action->text() == QStringLiteral("Rotate Right"))
                rotateRight = action;
    ASSERT_NE(rotateRight, nullptr) << "the video toolbar offers no rotation";

    rotateRight->trigger();
    rotateRight->trigger();
    EXPECT_EQ(view.videoRotation(), 180);

    // A different clip. It need not decode -- the stub engine does nothing with
    // it -- but it has to exist, or showFile never reaches the video branch.
    const QString next = dir.filePath(QStringLiteral("next.mp4"));
    QFile file(next);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("not really a clip");
    file.close();

    view.showFile(next);
    qApp->processEvents();
    EXPECT_EQ(view.videoRotation(), 0)
        << "the rotation corrected the previous clip and followed this one in";
}
