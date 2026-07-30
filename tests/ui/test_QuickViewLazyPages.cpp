#include <gtest/gtest.h>

#include <QElapsedTimer>
#include <QGraphicsView>
#include <QPalette>
#include <QPointer>
#include <QPushButton>
#include <QSlider>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QWidget>

#include <memory>
#include <string>

#include "ImagePreviewLoader.h"
#include "QuickView.h"
#include "config/Settings.h"
#include "media/MediaEngine.h"

namespace {

class CountingMediaEngine final : public MediaEngine {
public:
    void initialize() override { ++initializeCalls; }

    void load(const MediaSource &source, MediaKind kind) override {
        current = source;
        currentMediaKind = kind;
        ++loadCalls;
        currentState = MediaState::Playing;
        emit stateChanged(currentState);
    }

    void stop() override {
        current = {};
        currentState = MediaState::Idle;
        emit stateChanged(currentState);
    }

    QWidget *videoSurface() override {
        if (!surface)
            surface = new QWidget;
        return surface;
    }

    MediaState state() const override { return currentState; }
    MediaKind currentKind() const override { return currentMediaKind; }
    MediaSource currentSource() const override { return current; }
    void setVolume(int value) override { volume = value; }
    void setMute(bool value) override { muted = value; }

    int initializeCalls = 0;
    int loadCalls = 0;
    int volume = 100;
    bool muted = false;
    MediaSource current;
    MediaKind currentMediaKind = MediaKind::Audio;
    MediaState currentState = MediaState::Idle;

private:
    QPointer<QWidget> surface;
};

QWidget *page(QuickView &view, const char *name) {
    return view.findChild<QWidget *>(QString::fromLatin1(name));
}

} // namespace

TEST(QuickViewLazyPages, ConstructorKeepsHeavyPagesAbsentAndOneImageLoaderEager) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Settings settings(dir.filePath(QStringLiteral("settings.ini")));
    auto engine = std::make_unique<CountingMediaEngine>();

    QuickView view(settings, QuickView::Context::Embedded, nullptr, std::move(engine));

    EXPECT_EQ(view.findChildren<ImagePreviewLoader *>().size(), 1);
    EXPECT_TRUE(view.findChildren<MediaEngine *>().isEmpty());
    EXPECT_EQ(page(view, "quickViewVideoPage"), nullptr);
    EXPECT_EQ(page(view, "quickViewAudioPage"), nullptr);
    EXPECT_EQ(page(view, "quickViewPdfPage"), nullptr);
    EXPECT_EQ(page(view, "quickViewOfficePage"), nullptr);
    EXPECT_EQ(page(view, "quickViewArchivePage"), nullptr);
    EXPECT_EQ(page(view, "quickViewSlidesPage"), nullptr);
}

TEST(QuickViewLazyPages, WarmMediaEngineInitializesAndBuildsOneSharedBackendOnce) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Settings settings(dir.filePath(QStringLiteral("settings.ini")));
    auto engine = std::make_unique<CountingMediaEngine>();
    auto *counting = engine.get();
    QuickView view(settings, QuickView::Context::Embedded, nullptr, std::move(engine));

    view.warmMediaEngine();
    view.warmMediaEngine();

    EXPECT_EQ(counting->initializeCalls, 1);
    EXPECT_EQ(view.findChildren<MediaEngine *>().size(), 1);
    EXPECT_EQ(page(view, "quickViewVideoPage"), nullptr);
    EXPECT_EQ(page(view, "quickViewAudioPage"), nullptr);
}

TEST(QuickViewLazyPages, HeavyPageFactoriesCreateOnlyTheRequestedPage) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Settings settings(dir.filePath(QStringLiteral("settings.ini")));
    QuickView view(settings);

    EXPECT_NE(view.ensurePdfPage(), nullptr);
    EXPECT_NE(page(view, "quickViewPdfPage"), nullptr);
    EXPECT_EQ(page(view, "quickViewOfficePage"), nullptr);
    EXPECT_EQ(page(view, "quickViewArchivePage"), nullptr);
    EXPECT_EQ(page(view, "quickViewSlidesPage"), nullptr);

    EXPECT_NE(view.ensureOfficePage(), nullptr);
    EXPECT_NE(page(view, "quickViewOfficePage"), nullptr);
    EXPECT_EQ(page(view, "quickViewArchivePage"), nullptr);
    EXPECT_EQ(page(view, "quickViewSlidesPage"), nullptr);

    EXPECT_NE(view.ensureArchivePage(), nullptr);
    EXPECT_NE(page(view, "quickViewArchivePage"), nullptr);
    EXPECT_EQ(page(view, "quickViewSlidesPage"), nullptr);

    EXPECT_NE(view.ensureSlidesPage(), nullptr);
    EXPECT_NE(page(view, "quickViewSlidesPage"), nullptr);

    EXPECT_EQ(view.ensurePdfPage(), page(view, "quickViewPdfPage"));
    EXPECT_EQ(view.ensureOfficePage(), page(view, "quickViewOfficePage"));
    EXPECT_EQ(view.ensureArchivePage(), page(view, "quickViewArchivePage"));
    EXPECT_EQ(view.ensureSlidesPage(), page(view, "quickViewSlidesPage"));
}

TEST(QuickViewLazyPages, LatestSettingsApplyWhenPagesAreCreated) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Settings settings(dir.filePath(QStringLiteral("settings.ini")));
    settings.setVideoVolume(41);
    settings.setVideoMuted(true);
    settings.setAudioVolume(63);
    settings.setAudioMuted(false);
    auto engine = std::make_unique<CountingMediaEngine>();
    QuickView view(settings, QuickView::Context::Embedded, nullptr, std::move(engine));

    view.setContentFontFamily(QStringLiteral("Arial"));
    view.setContentFontSize(16);
    QPalette latestPalette = view.palette();
    latestPalette.setColor(QPalette::Window, QColor(17, 83, 129));
    view.setPalette(latestPalette);

    QWidget *office = view.ensureOfficePage();
    QWidget *pdf = view.ensurePdfPage();
    QWidget *archive = view.ensureArchivePage();
    QWidget *slides = view.ensureSlidesPage();
    view.warmMediaEngine();
    const QString videoPath = dir.filePath(QStringLiteral("settings.mp4"));
    QFile videoFile(videoPath);
    ASSERT_TRUE(videoFile.open(QIODevice::WriteOnly));
    videoFile.write("video");
    videoFile.close();
    const QString audioPath = dir.filePath(QStringLiteral("settings.wav"));
    QFile audioFile(audioPath);
    ASSERT_TRUE(audioFile.open(QIODevice::WriteOnly));
    audioFile.write("audio");
    audioFile.close();
    view.showFile(videoPath);
    view.showFile(audioPath);

    ASSERT_NE(office, nullptr);
    EXPECT_EQ(office->font().family(), QStringLiteral("Arial"));
    EXPECT_EQ(office->font().pointSize(), 16);
    for (QWidget *heavyPage : {office, pdf, archive, slides})
        EXPECT_EQ(heavyPage->palette().color(QPalette::Window), QColor(17, 83, 129));

    auto *videoVolume = view.findChild<QSlider *>(QStringLiteral("quickViewVideoVolume"));
    auto *videoMute = view.findChild<QPushButton *>(QStringLiteral("quickViewVideoMute"));
    auto *audioVolume = view.findChild<QSlider *>(QStringLiteral("quickViewAudioVolume"));
    auto *audioMute = view.findChild<QPushButton *>(QStringLiteral("quickViewAudioMute"));
    ASSERT_NE(videoVolume, nullptr);
    ASSERT_NE(videoMute, nullptr);
    ASSERT_NE(audioVolume, nullptr);
    ASSERT_NE(audioMute, nullptr);
    EXPECT_EQ(videoVolume->value(), 41);
    EXPECT_TRUE(videoMute->isChecked());
    EXPECT_EQ(audioVolume->value(), 63);
    EXPECT_FALSE(audioMute->isChecked());
}

TEST(QuickViewLazyPages, WarmedEngineHandlesRapidAudioVideoReplacementWithoutRecreation) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString audio = dir.filePath(QStringLiteral("first.wav"));
    const QString video = dir.filePath(QStringLiteral("second.mp4"));
    QFile audioFile(audio);
    ASSERT_TRUE(audioFile.open(QIODevice::WriteOnly));
    audioFile.write("audio");
    audioFile.close();
    QFile videoFile(video);
    ASSERT_TRUE(videoFile.open(QIODevice::WriteOnly));
    videoFile.write("video");
    videoFile.close();

    Settings settings(dir.filePath(QStringLiteral("settings.ini")));
    auto engine = std::make_unique<CountingMediaEngine>();
    auto *counting = engine.get();
    QuickView view(settings, QuickView::Context::Embedded, nullptr, std::move(engine));
    view.warmMediaEngine();

    QElapsedTimer elapsed;
    elapsed.start();
    view.showFile(audio);
    const qint64 firstPreviewMs = elapsed.elapsed();
    RecordProperty("first_media_preview_ms", std::to_string(firstPreviewMs));
    EXPECT_LT(firstPreviewMs, 50);
    EXPECT_NE(page(view, "quickViewAudioPage"), nullptr);
    EXPECT_EQ(page(view, "quickViewVideoPage"), nullptr);

    view.showFile(video);
    EXPECT_NE(page(view, "quickViewVideoPage"), nullptr);
    view.showFile(audio);

    EXPECT_EQ(counting->initializeCalls, 1);
    EXPECT_EQ(counting->loadCalls, 3);
    EXPECT_EQ(counting->current.path, audio);
    EXPECT_EQ(counting->currentMediaKind, MediaKind::Audio);
    EXPECT_EQ(view.findChildren<MediaEngine *>().size(), 1);
}
