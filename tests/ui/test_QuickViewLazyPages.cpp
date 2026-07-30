#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDataStream>
#include <QElapsedTimer>
#include <QEvent>
#include <QGraphicsView>
#include <QLabel>
#include <QPalette>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QSlider>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QVector>
#include <QWidget>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>

#include "ImagePreviewLoader.h"
#include "QuickView.h"
#include "config/Settings.h"
#include "media/MediaEngine.h"
#if FILECOMMANDER_HAS_PREVIEW_MEDIA
#include <mpv/client.h>

#include "media/MpvMediaEngine.h"
#endif

namespace {

class PaintObserver final : public QObject {
public:
    explicit PaintObserver(QElapsedTimer &elapsed) : elapsed(elapsed) {}

    qint64 firstPaintMs = -1;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() == QEvent::Paint && firstPaintMs < 0)
            firstPaintMs = elapsed.elapsed();
        return QObject::eventFilter(watched, event);
    }

private:
    QElapsedTimer &elapsed;
};

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

struct ThrowingEngineState {
    int initializeCalls = 0;
    int loadCalls = 0;
};

class ThrowingMediaEngine final : public MediaEngine {
public:
    explicit ThrowingMediaEngine(std::shared_ptr<ThrowingEngineState> state)
        : state(std::move(state)) {}

    void initialize() override {
        ++state->initializeCalls;
        throw std::runtime_error("test mpv initializer failed");
    }

    void load(const MediaSource &, MediaKind) override { ++state->loadCalls; }
    QWidget *videoSurface() override { return nullptr; }

private:
    std::shared_ptr<ThrowingEngineState> state;
};

QWidget *page(QuickView &view, const char *name) {
    return view.findChild<QWidget *>(QString::fromLatin1(name));
}

bool writePcmWav(const QString &path) {
    constexpr quint32 sampleRate = 8000;
    constexpr quint16 channels = 1;
    constexpr quint16 bitsPerSample = 16;
    constexpr quint32 sampleCount = sampleRate;
    constexpr quint32 dataSize = sampleCount * channels * (bitsPerSample / 8);

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    QDataStream output(&file);
    output.setByteOrder(QDataStream::LittleEndian);
    output.writeRawData("RIFF", 4);
    output << quint32(36 + dataSize);
    output.writeRawData("WAVEfmt ", 8);
    output << quint32(16) << quint16(1) << channels << sampleRate;
    output << quint32(sampleRate * channels * (bitsPerSample / 8));
    output << quint16(channels * (bitsPerSample / 8)) << bitsPerSample;
    output.writeRawData("data", 4);
    output << dataSize;
    for (quint32 sample = 0; sample < sampleCount; ++sample)
        output << qint16(0);
    return output.status() == QDataStream::Ok;
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

TEST(QuickViewLazyPages, InitializationExceptionBecomesStableActionableInfoState) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Settings settings(dir.filePath(QStringLiteral("settings.ini")));
    auto state = std::make_shared<ThrowingEngineState>();
    auto engine = std::make_unique<ThrowingMediaEngine>(state);
    QuickView view(settings, QuickView::Context::Embedded, nullptr, std::move(engine));
    QSignalSpy failed(&view, &QuickView::mediaEngineWarmFailed);

    EXPECT_NO_THROW(view.warmMediaEngine());
    EXPECT_NO_THROW(view.warmMediaEngine());

    auto *info = view.findChild<QLabel *>(QStringLiteral("previewInfoLabel"));
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(state->initializeCalls, 1);
    EXPECT_EQ(failed.count(), 1);
    EXPECT_TRUE(info->isVisibleTo(&view));
    EXPECT_TRUE(info->text().contains(QStringLiteral("test mpv initializer failed")));
    EXPECT_TRUE(info->text().contains(QStringLiteral("Restart File Commander")));
    EXPECT_EQ(page(view, "quickViewVideoPage"), nullptr);
    EXPECT_EQ(page(view, "quickViewAudioPage"), nullptr);
}

TEST(QuickViewLazyPages, FirstUseFailureDoesNotRetryOrBuildMediaPages) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString audio = dir.filePath(QStringLiteral("failed.wav"));
    const QString video = dir.filePath(QStringLiteral("failed.mp4"));
    QFile audioFile(audio);
    ASSERT_TRUE(audioFile.open(QIODevice::WriteOnly));
    audioFile.write("audio");
    audioFile.close();
    QFile videoFile(video);
    ASSERT_TRUE(videoFile.open(QIODevice::WriteOnly));
    videoFile.write("video");
    videoFile.close();

    Settings settings(dir.filePath(QStringLiteral("settings.ini")));
    auto state = std::make_shared<ThrowingEngineState>();
    auto engine = std::make_unique<ThrowingMediaEngine>(state);
    QuickView view(settings, QuickView::Context::Embedded, nullptr, std::move(engine));

    EXPECT_NO_THROW(view.showFile(audio));
    EXPECT_NO_THROW(view.showFile(video));
    EXPECT_NO_THROW(view.showFile(audio));

    EXPECT_EQ(state->initializeCalls, 1);
    EXPECT_EQ(state->loadCalls, 0);
    EXPECT_EQ(page(view, "quickViewVideoPage"), nullptr);
    EXPECT_EQ(page(view, "quickViewAudioPage"), nullptr);
    auto *info = view.findChild<QLabel *>(QStringLiteral("previewInfoLabel"));
    ASSERT_NE(info, nullptr);
    EXPECT_TRUE(info->isVisibleTo(&view));
    EXPECT_TRUE(info->text().contains(QStringLiteral("Restart File Commander")));
}

TEST(QuickViewLazyPages, TimerWarmFailureDoesNotEscapeAndTeardownIsSafe) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Settings settings(dir.filePath(QStringLiteral("settings.ini")));
    auto state = std::make_shared<ThrowingEngineState>();
    auto engine = std::make_unique<ThrowingMediaEngine>(state);
    auto *view =
        new QuickView(settings, QuickView::Context::Embedded, nullptr, std::move(engine));
    QPointer<QuickView> guard(view);
    bool escapedCallback = false;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, view, [view, &escapedCallback]() {
        try {
            view->warmMediaEngine();
        } catch (...) {
            escapedCallback = true;
        }
    });

    timer.start(0);
    QTRY_COMPARE_WITH_TIMEOUT(state->initializeCalls, 1, 1000);
    EXPECT_FALSE(escapedCallback);
    delete view;
    QCoreApplication::processEvents();

    EXPECT_TRUE(guard.isNull());
    EXPECT_FALSE(timer.isActive());
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

#if FILECOMMANDER_HAS_PREVIEW_MEDIA
TEST(QuickViewLazyPages, RealMpvInitializeExceptionIsContainedByQuickView) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Settings settings(dir.filePath(QStringLiteral("settings.ini")));
    MpvMediaEngine::Options options;
    options.createContext = []() { return mpv_create(); };
    options.initializeContext = [](mpv_handle *) { return -1; };
    auto engine = std::make_unique<MpvMediaEngine>(std::move(options));
    QuickView view(settings, QuickView::Context::Embedded, nullptr, std::move(engine));
    QSignalSpy failed(&view, &QuickView::mediaEngineWarmFailed);

    EXPECT_NO_THROW(view.warmMediaEngine());
    EXPECT_NO_THROW(view.warmMediaEngine());

    auto *info = view.findChild<QLabel *>(QStringLiteral("previewInfoLabel"));
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(failed.count(), 1);
    EXPECT_TRUE(info->text().contains(QStringLiteral("could not initialize mpv")));
    EXPECT_TRUE(info->text().contains(QStringLiteral("Restart File Commander")));
    EXPECT_EQ(view.findChildren<MpvMediaEngine *>().size(), 1);
}

TEST(QuickViewLazyPages, RealMpvWarmupDistributionRecordsGuiThreadCost) {
    constexpr int sampleCount = 20;
    QVector<qint64> samples;
    samples.reserve(sampleCount);

    for (int sample = 0; sample < sampleCount; ++sample) {
        QTemporaryDir dir;
        ASSERT_TRUE(dir.isValid());
        Settings settings(dir.filePath(QStringLiteral("settings.ini")));
        QuickView view(settings);
        QSignalSpy warmed(&view, &QuickView::mediaEngineWarmed);

        view.warmMediaEngine();

        ASSERT_EQ(warmed.count(), 1);
        samples.append(warmed.at(0).at(0).toLongLong());
    }

    std::sort(samples.begin(), samples.end());
    const qint64 minimum = samples.first();
    const qint64 median = samples.at(sampleCount / 2);
    const qint64 p95 = samples.at(18);
    const qint64 maximum = samples.last();
    RecordProperty("warm_samples", std::to_string(sampleCount));
    RecordProperty("warm_min_ms", std::to_string(minimum));
    RecordProperty("warm_median_ms", std::to_string(median));
    RecordProperty("warm_p95_ms", std::to_string(p95));
    RecordProperty("warm_max_ms", std::to_string(maximum));

    EXPECT_GE(maximum, minimum);
}

TEST(QuickViewLazyPages, RealMpvFirstAudioPreviewIsDecodedVisibleAndPainted) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString audio = dir.filePath(QStringLiteral("real-preview.wav"));
    ASSERT_TRUE(writePcmWav(audio));
    Settings settings(dir.filePath(QStringLiteral("settings.ini")));
    auto pendingEngine = std::make_unique<MpvMediaEngine>();
    auto *engine = pendingEngine.get();
    QuickView view(settings, QuickView::Context::Embedded, nullptr,
                   std::move(pendingEngine));
    view.resize(720, 480);
    view.show();
    QTest::qWaitForWindowExposed(&view);
    EXPECT_TRUE(view.findChildren<MpvMediaEngine *>().isEmpty());
    QSignalSpy warmed(&view, &QuickView::mediaEngineWarmed);

    QElapsedTimer elapsed;
    qint64 decodedMs = -1;
    QObject::connect(engine, &MediaEngine::stateChanged, &view,
                     [&elapsed, &decodedMs](MediaState state) {
                         if (state == MediaState::Playing && decodedMs < 0)
                             decodedMs = elapsed.elapsed();
                     });
    elapsed.start();
    view.showFile(audio);
    ASSERT_EQ(warmed.count(), 1);
    ASSERT_EQ(view.findChildren<MpvMediaEngine *>().size(), 1);
    auto *audioPage = page(view, "quickViewAudioPage");
    ASSERT_NE(audioPage, nullptr);
    PaintObserver paintObserver(elapsed);
    audioPage->installEventFilter(&paintObserver);

    QTRY_VERIFY_WITH_TIMEOUT(decodedMs >= 0, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(paintObserver.firstPaintMs >= 0, 1000);
    ASSERT_TRUE(audioPage->isVisibleTo(&view));
    const qint64 previewMs = std::max(decodedMs, paintObserver.firstPaintMs);
    RecordProperty("first_use_warm_ms",
                   std::to_string(warmed.at(0).at(0).toLongLong()));
    RecordProperty("real_audio_decoded_ms", std::to_string(decodedMs));
    RecordProperty("real_audio_first_paint_ms",
                   std::to_string(paintObserver.firstPaintMs));
    RecordProperty("real_audio_preview_ms", std::to_string(previewMs));
    EXPECT_LT(previewMs, 1000);
}
#endif
