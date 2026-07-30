#include <gtest/gtest.h>

#include <QFile>
#include <QGraphicsOpacityEffect>
#include <QImage>
#include <QLabel>
#include <QOpenGLWidget>
#include <QPainter>
#include <QPdfWriter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QScrollArea>
#include <QSemaphore>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBrowser>

#include <memory>

#include "ImagePreviewLoader.h"
#include "MotionPolicy.h"
#include "QuickView.h"
#include "config/Settings.h"
#include "media/MediaEngine.h"

namespace {

class MotionOverride final {
public:
  explicit MotionOverride(bool reduced) {
    MotionPolicy::setReducedForTest(reduced);
  }
  ~MotionOverride() { MotionPolicy::clearReducedForTest(); }
};

class TestMediaEngine final : public MediaEngine {
public:
  void initialize() override { ++initializeCalls; }

  void load(const MediaSource &source, MediaKind kind) override {
    current = source;
    currentMediaKind = kind;
    currentState = MediaState::Playing;
  }

  void stop() override {
    current = {};
    currentState = MediaState::Idle;
  }

  QWidget *videoSurface() override {
    if (!surface)
      surface = new QWidget;
    return surface;
  }

  MediaState state() const override { return currentState; }
  MediaKind currentKind() const override { return currentMediaKind; }
  MediaSource currentSource() const override { return current; }

  int initializeCalls = 0;
  MediaSource current;
  MediaKind currentMediaKind = MediaKind::Audio;
  MediaState currentState = MediaState::Idle;

private:
  QPointer<QWidget> surface;
};

QStackedWidget *previewStack(QuickView &view) {
  return view.findChild<QStackedWidget *>();
}

QWidget *namedPage(QuickView &view, const char *name) {
  return view.findChild<QWidget *>(QString::fromLatin1(name));
}

QWidget *imagePage(QuickView &view) {
  auto *scroll =
      view.findChild<QScrollArea *>(QStringLiteral("imagePreviewScroll"));
  return scroll ? scroll->parentWidget() : nullptr;
}

QWidget *textPage(QuickView &view) {
  auto *editor = view.findChild<QPlainTextEdit *>();
  return editor ? editor->parentWidget() : nullptr;
}

QWidget *markdownPage(QuickView &view) {
  return view.findChild<QTextBrowser *>();
}

QString writeImage(const QTemporaryDir &dir, const QString &name,
                   const QColor &color) {
  QImage image(QSize(96, 64), QImage::Format_ARGB32);
  image.fill(color);
  const QString path = dir.filePath(name);
  return image.save(path, "PNG") ? path : QString();
}

QString writeText(const QTemporaryDir &dir, const QString &name,
                  const QByteArray &contents) {
  const QString path = dir.filePath(name);
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly) ||
      file.write(contents) != contents.size())
    return {};
  return path;
}

void expectFadeStarted(QWidget *page) {
  ASSERT_NE(page, nullptr);
  auto *effect = qobject_cast<QGraphicsOpacityEffect *>(page->graphicsEffect());
  ASSERT_NE(effect, nullptr);
  EXPECT_GE(effect->opacity(), 0.85);
  EXPECT_LT(effect->opacity(), 1.0);
}

TEST(QuickViewMotion, ApprovedStaticPagesUseOnlyTemporaryOpacity) {
  MotionOverride motion(false);
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  Settings settings(dir.filePath(QStringLiteral("settings.ini")));
  QuickView view(settings);
  view.resize(720, 520);
  view.show();

  auto *info = view.findChild<QLabel *>(QStringLiteral("previewInfoLabel"));
  auto *pdf = view.ensurePdfPage();
  auto *office = view.ensureOfficePage();
  auto *archive = view.ensureArchivePage();
  auto *slides = view.ensureSlidesPage();
  const QList<QWidget *> pages = {
      imagePage(view), textPage(view), markdownPage(view), pdf, office, archive,
      slides,          info,
  };

  for (QWidget *page : pages) {
    ASSERT_NE(page, nullptr);
    previewStack(view)->setCurrentWidget(page);
    QCoreApplication::processEvents();
    const QRect geometryBefore = page->geometry();
    view.revealStaticPage(page);
    EXPECT_EQ(previewStack(view)->currentWidget(), page);
    expectFadeStarted(page);
    EXPECT_EQ(page->geometry(), geometryBefore);
    EXPECT_TRUE(page->isEnabled());
  }

  QTest::qWait(130);
  EXPECT_EQ(pages.last()->graphicsEffect(), nullptr);
}

TEST(QuickViewMotion, ReducedMotionShowsFinalStaticStateImmediately) {
  MotionOverride motion(false);
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  Settings settings(dir.filePath(QStringLiteral("settings.ini")));
  QuickView view(settings);
  QWidget *page = textPage(view);
  ASSERT_NE(page, nullptr);

  view.revealStaticPage(page);
  expectFadeStarted(page);

  MotionPolicy::setReducedForTest(true);

  EXPECT_EQ(previewStack(view)->currentWidget(), page);
  EXPECT_EQ(page->graphicsEffect(), nullptr);
  EXPECT_TRUE(page->isEnabled());

  view.revealStaticPage(page);
  EXPECT_EQ(page->graphicsEffect(), nullptr);
}

TEST(QuickViewMotion, MediaOpenGLAndProgressPagesNeverReceiveOpacityEffects) {
  MotionOverride motion(false);
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString audio =
      writeText(dir, QStringLiteral("sound.wav"), QByteArrayLiteral("audio"));
  const QString video =
      writeText(dir, QStringLiteral("movie.mp4"), QByteArrayLiteral("video"));
  ASSERT_FALSE(audio.isEmpty());
  ASSERT_FALSE(video.isEmpty());

  Settings settings(dir.filePath(QStringLiteral("settings.ini")));
  auto engine = std::make_unique<TestMediaEngine>();
  auto *engineState = engine.get();
  QuickView view(settings, QuickView::Context::Embedded, nullptr,
                 std::move(engine));
  view.resize(720, 520);
  view.show();

  view.showFile(audio);
  QWidget *audioPage = namedPage(view, "quickViewAudioPage");
  ASSERT_NE(audioPage, nullptr);
  EXPECT_EQ(audioPage->graphicsEffect(), nullptr);

  view.showFile(video);
  QWidget *videoPage = namedPage(view, "quickViewVideoPage");
  ASSERT_NE(videoPage, nullptr);
  EXPECT_EQ(videoPage->graphicsEffect(), nullptr);
  ASSERT_NE(engineState->videoSurface(), nullptr);
  EXPECT_EQ(engineState->videoSurface()->graphicsEffect(), nullptr);
  EXPECT_EQ(engineState->initializeCalls, 1);

  QWidget *openGlPage = imagePage(view);
  ASSERT_NE(openGlPage, nullptr);
  auto *openGlSurface = new QOpenGLWidget(openGlPage);
  view.revealStaticPage(openGlPage);
  EXPECT_EQ(openGlPage->graphicsEffect(), nullptr);
  EXPECT_EQ(openGlSurface->graphicsEffect(), nullptr);

  view.showDownloading(QStringLiteral("remote.bin"));
  QWidget *downloadPage = previewStack(view)->currentWidget();
  ASSERT_NE(downloadPage, nullptr);
  EXPECT_NE(downloadPage->findChild<QProgressBar *>(), nullptr);
  EXPECT_EQ(downloadPage->graphicsEffect(), nullptr);
}

TEST(QuickViewMotion,
     LatestImageGenerationKeepsOldPageUntilReadyAndOwnsFinalFade) {
  MotionOverride motion(false);
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString oldText = writeText(dir, QStringLiteral("old.txt"),
                                    QByteArrayLiteral("old preview"));
  const QString first = writeImage(dir, QStringLiteral("first.png"), Qt::red);
  const QString second =
      writeImage(dir, QStringLiteral("second.png"), Qt::blue);
  const QString final = writeImage(dir, QStringLiteral("final.png"), Qt::green);
  ASSERT_FALSE(oldText.isEmpty());
  ASSERT_FALSE(first.isEmpty());
  ASSERT_FALSE(second.isEmpty());
  ASSERT_FALSE(final.isEmpty());

  Settings settings(dir.filePath(QStringLiteral("settings.ini")));
  QuickView view(settings);
  view.resize(720, 520);
  view.show();
  view.showFile(oldText);
  QTest::qWait(130);
  QWidget *oldPage = previewStack(view)->currentWidget();
  ASSERT_EQ(oldPage, textPage(view));

  auto *loader = view.findChild<ImagePreviewLoader *>();
  ASSERT_NE(loader, nullptr);
  QSemaphore firstLoadEntered;
  QSemaphore releaseFirstLoad;
  bool heldFirst = false;
  loader->setWorkerCheckpointForTest(
      [&](ImagePreviewLoader::WorkerCheckpoint checkpoint, quint64) {
        if (checkpoint ==
                ImagePreviewLoader::WorkerCheckpoint::LoadBeforeDecode &&
            !heldFirst) {
          heldFirst = true;
          firstLoadEntered.release();
          releaseFirstLoad.acquire();
        }
      });

  view.showFile(first);
  ASSERT_TRUE(firstLoadEntered.tryAcquire(1, 5000));
  view.showFile(second);
  view.showFile(final);
  EXPECT_EQ(previewStack(view)->currentWidget(), oldPage);
  EXPECT_EQ(oldPage->graphicsEffect(), nullptr);

  releaseFirstLoad.release();
  ASSERT_TRUE(loader->waitForIdleForTest(5000));
  QTRY_COMPARE_WITH_TIMEOUT(previewStack(view)->currentWidget(),
                            imagePage(view), 1000);
  expectFadeStarted(imagePage(view));

  auto *scroll =
      view.findChild<QScrollArea *>(QStringLiteral("imagePreviewScroll"));
  auto *label = scroll ? qobject_cast<QLabel *>(scroll->widget()) : nullptr;
  ASSERT_NE(label, nullptr);
  ASSERT_NE(label->pixmap(), nullptr);
  const QImage displayed = label->pixmap()->toImage();
  EXPECT_EQ(displayed.pixelColor(displayed.width() / 2, displayed.height() / 2),
            QColor(Qt::green));

  QTest::qWait(130);
  EXPECT_EQ(imagePage(view)->graphicsEffect(), nullptr);
}

#if FILECOMMANDER_HAS_PREVIEW_PDF
TEST(QuickViewMotion, PdfKeepsOldPageUntilDocumentIsReady) {
  MotionOverride motion(false);
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString oldText = writeText(dir, QStringLiteral("old.txt"),
                                    QByteArrayLiteral("old preview"));
  const QString pdfPath = dir.filePath(QStringLiteral("ready.pdf"));
  {
    QPdfWriter writer(pdfPath);
    writer.setResolution(72);
    QPainter painter(&writer);
    ASSERT_TRUE(painter.isActive());
    painter.drawText(QPoint(72, 72), QStringLiteral("ready"));
  }

  Settings settings(dir.filePath(QStringLiteral("settings.ini")));
  QuickView view(settings);
  view.resize(720, 520);
  view.show();
  view.showFile(oldText);
  QTest::qWait(130);
  QWidget *oldPage = previewStack(view)->currentWidget();
  ASSERT_EQ(oldPage, textPage(view));

  view.showFile(pdfPath);

  EXPECT_EQ(previewStack(view)->currentWidget(), oldPage);
  QTRY_COMPARE_WITH_TIMEOUT(previewStack(view)->currentWidget(),
                            namedPage(view, "quickViewPdfPage"), 5000);
  expectFadeStarted(namedPage(view, "quickViewPdfPage"));
}
#endif

TEST(QuickViewMotion, ThemeRefreshAndTeardownAreSafeDuringReveal) {
  MotionOverride motion(false);
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  Settings settings(dir.filePath(QStringLiteral("settings.ini")));
  auto *view = new QuickView(settings);
  view->resize(720, 520);
  view->show();
  QWidget *page = textPage(*view);
  ASSERT_NE(page, nullptr);

  view->revealStaticPage(page);
  expectFadeStarted(page);
  EXPECT_NO_THROW(view->refreshPhosphor());
  expectFadeStarted(page);

  QPointer<QuickView> guard(view);
  delete view;
  QTest::qWait(130);
  EXPECT_TRUE(guard.isNull());
}

} // namespace
