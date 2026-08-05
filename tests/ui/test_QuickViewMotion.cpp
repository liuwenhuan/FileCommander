#include <gtest/gtest.h>

#include <QFile>
#include <QGraphicsOpacityEffect>
#include <QGraphicsView>
#include <QHeaderView>
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
#include <QTableView>
#include <QTableWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QToolBar>

#include <memory>

#include "ImagePreviewLoader.h"
#include "MotionPolicy.h"
#include "QuickView.h"
#include "ArchiveHandler.h"
#include "config/Settings.h"
#include "media/MediaEngine.h"
#include "ThemeStateGuard.h"

namespace {

class MotionOverride final {
public:
  explicit MotionOverride(bool reduced) {
    MotionPolicy::setReducedForTest(reduced);
  }
  ~MotionOverride() { MotionPolicy::clearReducedForTest(); }
};

class EnvironmentOverride final {
public:
  EnvironmentOverride(const char *name, const QByteArray &value)
      : name_(name), existed_(qEnvironmentVariableIsSet(name)),
        previous_(qgetenv(name)) {
    qputenv(name, value);
  }

  ~EnvironmentOverride() {
    if (existed_)
      qputenv(name_.constData(), previous_);
    else
      qunsetenv(name_.constData());
  }

private:
  QByteArray name_;
  bool existed_;
  QByteArray previous_;
};

class OfficeFixtureEnvironment final {
public:
  explicit OfficeFixtureEnvironment(int delayMs = 0)
      : binary_("TTC_OFFICE_OXIDE", QByteArray(TTC_OFFICE_FIXTURE_CLI)),
        delay_("TTC_OFFICE_FIXTURE_DELAY_MS",
               QByteArray::number(delayMs)) {}

private:
  EnvironmentOverride binary_;
  EnvironmentOverride delay_;
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

QTextBrowser *markdownPage(QuickView &view) {
  return view.findChild<QTextBrowser *>();
}

QWidget *scrollContent(QWidget *scrollable) {
  auto *area = qobject_cast<QAbstractScrollArea *>(scrollable);
  return area ? area->viewport() : nullptr;
}

QAction *actionByText(QWidget &root, const QString &text) {
  for (QAction *action : root.findChildren<QAction *>()) {
    if (action->text() == text)
      return action;
  }
  return nullptr;
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
  auto *archive = view.ensureArchivePage();
  auto *slides = view.ensureSlidesPage();
  struct StaticSurface {
    QWidget *page;
    QWidget *content;
  };
  const QList<StaticSurface> surfaces = {
      {imagePage(view),
       scrollContent(view.findChild<QScrollArea *>(
           QStringLiteral("imagePreviewScroll")))},
      {textPage(view), scrollContent(view.findChild<QPlainTextEdit *>())},
      {markdownPage(view), scrollContent(markdownPage(view))},
      {pdf, scrollContent(pdf->findChild<QGraphicsView *>())},
      {archive, scrollContent(archive->findChild<QTableView *>())},
      {slides, scrollContent(slides->findChild<QGraphicsView *>())},
      {info, info},
  };

  for (const StaticSurface &surface : surfaces) {
    ASSERT_NE(surface.page, nullptr);
    ASSERT_NE(surface.content, nullptr);
    previewStack(view)->setCurrentWidget(surface.page);
    QCoreApplication::processEvents();
    const QRect geometryBefore = surface.page->geometry();
    view.revealStaticPage(surface.page);
    EXPECT_EQ(previewStack(view)->currentWidget(), surface.page);
    if (surface.page != surface.content)
      EXPECT_EQ(surface.page->graphicsEffect(), nullptr);
    expectFadeStarted(surface.content);
    EXPECT_EQ(surface.page->geometry(), geometryBefore);
    EXPECT_TRUE(surface.page->isEnabled());
    for (QToolBar *toolbar : surface.page->findChildren<QToolBar *>()) {
      EXPECT_EQ(toolbar->graphicsEffect(), nullptr);
      EXPECT_TRUE(toolbar->isEnabled());
    }
  }

  QTest::qWait(130);
  EXPECT_EQ(surfaces.last().content->graphicsEffect(), nullptr);
}

TEST(QuickViewMotion, ReducedMotionShowsFinalStaticStateImmediately) {
  MotionOverride motion(false);
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  Settings settings(dir.filePath(QStringLiteral("settings.ini")));
  QuickView view(settings);
  QWidget *page = textPage(view);
  QWidget *content = scrollContent(view.findChild<QPlainTextEdit *>());
  ASSERT_NE(page, nullptr);
  ASSERT_NE(content, nullptr);

  view.revealStaticPage(page);
  expectFadeStarted(content);

  MotionPolicy::setReducedForTest(true);

  EXPECT_EQ(previewStack(view)->currentWidget(), page);
  EXPECT_EQ(content->graphicsEffect(), nullptr);
  EXPECT_TRUE(page->isEnabled());

  view.revealStaticPage(page);
  EXPECT_EQ(content->graphicsEffect(), nullptr);
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
  // This test is about WHICH generation owns the final fade, and it reads the
  // answer off a pixel. The preview tint is process-wide and recolours that
  // pixel, so the test states what it needs instead of inheriting whatever the
  // previous test applied: a green PNG came back as (105, 129, 161) -- green
  // through a theme's content tint -- for every full-suite run.
  ThemeStateGuard themeState;
  fc::setPreviewTint(QColor());
  fc::setThumbnailTint(QColor());
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

  auto *scroll =
      view.findChild<QScrollArea *>(QStringLiteral("imagePreviewScroll"));
  expectFadeStarted(scrollContent(scroll));
  auto *label = scroll ? qobject_cast<QLabel *>(scroll->widget()) : nullptr;
  ASSERT_NE(label, nullptr);
  ASSERT_NE(label->pixmap(), nullptr);
  const QImage displayed = label->pixmap()->toImage();
  EXPECT_EQ(displayed.pixelColor(displayed.width() / 2, displayed.height() / 2),
            QColor(Qt::green));

  QTest::qWait(130);
  EXPECT_EQ(scrollContent(scroll)->graphicsEffect(), nullptr);
}

TEST(QuickViewMotion, MarkdownRouteKeepsOldPageUntilContentIsReady) {
  MotionOverride motion(false);
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString oldText = writeText(dir, QStringLiteral("old.txt"),
                                    QByteArrayLiteral("old preview"));
  const QString markdown =
      writeText(dir, QStringLiteral("ready.md"),
                QByteArrayLiteral("# Accepted Markdown\n\nRoute fixture."));
  ASSERT_FALSE(oldText.isEmpty());
  ASSERT_FALSE(markdown.isEmpty());

  Settings settings(dir.filePath(QStringLiteral("settings.ini")));
  QuickView view(settings);
  view.resize(720, 520);
  view.show();
  view.showFile(oldText);
  QTest::qWait(130);
  QWidget *oldPage = previewStack(view)->currentWidget();
  ASSERT_EQ(oldPage, textPage(view));

  view.showFile(markdown);

  EXPECT_EQ(previewStack(view)->currentWidget(), oldPage);
  QTRY_COMPARE_WITH_TIMEOUT(previewStack(view)->currentWidget(),
                            markdownPage(view), 5000);
  auto *browser = markdownPage(view);
  ASSERT_NE(browser, nullptr);
  EXPECT_TRUE(browser->toPlainText().contains(QStringLiteral("Accepted Markdown")));
  EXPECT_EQ(browser->graphicsEffect(), nullptr);
  expectFadeStarted(browser->viewport());
  EXPECT_NO_THROW(view.refreshPhosphor());
  expectFadeStarted(browser->viewport());
}

TEST(QuickViewMotion, ArchiveRouteKeepsOldPageUntilListingIsReady) {
  MotionOverride motion(false);
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString oldText = writeText(dir, QStringLiteral("old.txt"),
                                    QByteArrayLiteral("old preview"));
  const QString first = writeText(dir, QStringLiteral("first.txt"),
                                  QByteArrayLiteral("first"));
  const QString second = writeText(dir, QStringLiteral("second.txt"),
                                   QByteArrayLiteral("second"));
  const QString archive = dir.filePath(QStringLiteral("ready.zip"));
  QString archiveError;
  ASSERT_TRUE(ArchiveHandler::create(
      archive, {first, second}, QStringLiteral("zip"), &archiveError))
      << archiveError.toStdString();

  Settings settings(dir.filePath(QStringLiteral("settings.ini")));
  QuickView view(settings);
  view.resize(720, 520);
  view.show();
  view.showFile(oldText);
  QTest::qWait(130);
  QWidget *oldPage = previewStack(view)->currentWidget();

  view.showFile(archive);

  EXPECT_EQ(previewStack(view)->currentWidget(), oldPage);
  QWidget *archivePage = view.ensureArchivePage();
  QTRY_COMPARE_WITH_TIMEOUT(previewStack(view)->currentWidget(), archivePage,
                            5000);
  auto *table = archivePage->findChild<QTableView *>();
  ASSERT_NE(table, nullptr);
  EXPECT_GE(table->model()->rowCount(), 2);
  EXPECT_EQ(archivePage->graphicsEffect(), nullptr);
  EXPECT_EQ(table->graphicsEffect(), nullptr);
  expectFadeStarted(table->viewport());
  EXPECT_NO_THROW(view.refreshPhosphor());
  expectFadeStarted(table->viewport());
  for (QToolBar *toolbar : archivePage->findChildren<QToolBar *>())
    EXPECT_EQ(toolbar->graphicsEffect(), nullptr);
}

TEST(QuickViewMotion, OfficeDocumentRouteRevealsMarkdownContentOnly) {
  MotionOverride motion(false);
  OfficeFixtureEnvironment office;
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString oldText = writeText(dir, QStringLiteral("old.txt"),
                                    QByteArrayLiteral("old preview"));
  const QString document = writeText(dir, QStringLiteral("ready.docx"),
                                     QByteArrayLiteral("fixture"));

  Settings settings(dir.filePath(QStringLiteral("settings.ini")));
  QuickView view(settings);
  view.resize(720, 520);
  view.show();
  view.showFile(oldText);
  QTest::qWait(130);
  QWidget *oldPage = previewStack(view)->currentWidget();

  view.showFile(document);

  EXPECT_EQ(previewStack(view)->currentWidget(), oldPage);
  QTRY_COMPARE_WITH_TIMEOUT(previewStack(view)->currentWidget(),
                            markdownPage(view), 5000);
  auto *browser = markdownPage(view);
  ASSERT_NE(browser, nullptr);
  EXPECT_TRUE(browser->toPlainText().contains(QStringLiteral("Fixture document")));
  EXPECT_EQ(browser->graphicsEffect(), nullptr);
  expectFadeStarted(browser->viewport());
  EXPECT_NO_THROW(view.refreshPhosphor());
  expectFadeStarted(browser->viewport());
}

TEST(QuickViewMotion, OfficeGridRouteKeepsTabsAndGridControlsOpaque) {
  MotionOverride motion(false);
  OfficeFixtureEnvironment office;
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString oldText = writeText(dir, QStringLiteral("old.txt"),
                                    QByteArrayLiteral("old preview"));
  const QString workbook = writeText(dir, QStringLiteral("ready.xlsx"),
                                     QByteArrayLiteral("fixture"));

  Settings settings(dir.filePath(QStringLiteral("settings.ini")));
  QuickView view(settings);
  view.resize(720, 520);
  view.show();
  view.showFile(oldText);
  QTest::qWait(130);
  QWidget *oldPage = previewStack(view)->currentWidget();

  view.showFile(workbook);

  EXPECT_EQ(previewStack(view)->currentWidget(), oldPage);
  auto *officePage = qobject_cast<QTabWidget *>(
      view.ensureOfficePage());
  ASSERT_NE(officePage, nullptr);
  QTRY_COMPARE_WITH_TIMEOUT(previewStack(view)->currentWidget(), officePage,
                            5000);
  auto *grid = qobject_cast<QTableWidget *>(officePage->currentWidget());
  ASSERT_NE(grid, nullptr);
  EXPECT_EQ(grid->item(1, 1)->text(), QStringLiteral("42"));
  EXPECT_EQ(officePage->graphicsEffect(), nullptr);
  EXPECT_EQ(officePage->tabBar()->graphicsEffect(), nullptr);
  EXPECT_EQ(grid->graphicsEffect(), nullptr);
  EXPECT_EQ(grid->horizontalHeader()->graphicsEffect(), nullptr);
  EXPECT_EQ(grid->verticalHeader()->graphicsEffect(), nullptr);
  EXPECT_TRUE(grid->isEnabled());
  expectFadeStarted(grid->viewport());
  EXPECT_NO_THROW(view.refreshPhosphor());
  expectFadeStarted(grid->viewport());
}

TEST(QuickViewMotion, SlidesRouteKeepsToolbarOpaqueUntilAcceptedDeckIsReady) {
  MotionOverride motion(false);
  OfficeFixtureEnvironment office;
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString oldText = writeText(dir, QStringLiteral("old.txt"),
                                    QByteArrayLiteral("old preview"));
  const QString slides = writeText(dir, QStringLiteral("ready.pptx"),
                                   QByteArrayLiteral("fixture"));

  Settings settings(dir.filePath(QStringLiteral("settings.ini")));
  QuickView view(settings);
  view.resize(720, 520);
  view.show();
  view.showFile(oldText);
  QTest::qWait(130);
  QWidget *oldPage = previewStack(view)->currentWidget();

  view.showFile(slides);

  EXPECT_EQ(previewStack(view)->currentWidget(), oldPage);
  QWidget *slidesPage = view.ensureSlidesPage();
  QTRY_COMPARE_WITH_TIMEOUT(previewStack(view)->currentWidget(), slidesPage,
                            5000);
  auto *graphics = slidesPage->findChild<QGraphicsView *>();
  ASSERT_NE(graphics, nullptr);
  EXPECT_FALSE(graphics->scene()->items().isEmpty());
  EXPECT_EQ(slidesPage->graphicsEffect(), nullptr);
  expectFadeStarted(graphics->viewport());
  EXPECT_NO_THROW(view.refreshPhosphor());
  expectFadeStarted(graphics->viewport());
  for (QToolBar *toolbar : slidesPage->findChildren<QToolBar *>()) {
    EXPECT_EQ(toolbar->graphicsEffect(), nullptr);
    EXPECT_TRUE(toolbar->isEnabled());
  }
}

TEST(QuickViewMotion, FailedImageRouteKeepsOldPageUntilInfoIsReady) {
  MotionOverride motion(false);
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString oldText = writeText(dir, QStringLiteral("old.txt"),
                                    QByteArrayLiteral("old preview"));
  const QString damaged = writeText(dir, QStringLiteral("damaged.png"),
                                    QByteArrayLiteral("not an image"));

  Settings settings(dir.filePath(QStringLiteral("settings.ini")));
  QuickView view(settings);
  view.resize(720, 520);
  view.show();
  view.showFile(oldText);
  QTest::qWait(130);
  QWidget *oldPage = previewStack(view)->currentWidget();

  view.showFile(damaged);

  EXPECT_EQ(previewStack(view)->currentWidget(), oldPage);
  auto *info = view.findChild<QLabel *>(QStringLiteral("previewInfoLabel"));
  ASSERT_NE(info, nullptr);
  QTRY_COMPARE_WITH_TIMEOUT(previewStack(view)->currentWidget(), info, 5000);
  EXPECT_TRUE(info->text().contains(QStringLiteral("damaged.png")));
  expectFadeStarted(info);
}

TEST(QuickViewMotion, PendingImageRenderIsSafeDuringThemeRefreshAndTeardown) {
  MotionOverride motion(false);
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString image =
      writeImage(dir, QStringLiteral("pending.png"), QColor(Qt::cyan));
  ASSERT_FALSE(image.isEmpty());

  Settings settings(dir.filePath(QStringLiteral("settings.ini")));
  auto *view = new QuickView(settings);
  view->resize(720, 520);
  view->show();
  auto *loader = view->findChild<ImagePreviewLoader *>();
  ASSERT_NE(loader, nullptr);
  view->showFile(image);
  ASSERT_TRUE(loader->waitForIdleForTest(5000));

  QSemaphore renderEntered;
  QSemaphore releaseRender;
  bool heldRender = false;
  loader->setWorkerCheckpointForTest(
      [&](ImagePreviewLoader::WorkerCheckpoint checkpoint, quint64) {
        if (checkpoint ==
                ImagePreviewLoader::WorkerCheckpoint::RenderBeforeTransform &&
            !heldRender) {
          heldRender = true;
          renderEntered.release();
          releaseRender.acquire();
        }
      });

  QAction *zoomIn = actionByText(*view, QStringLiteral("Zoom In"));
  ASSERT_NE(zoomIn, nullptr);
  zoomIn->trigger();
  ASSERT_TRUE(renderEntered.tryAcquire(1, 5000));
  EXPECT_NO_THROW(view->refreshPhosphor());

  QPointer<QuickView> guard(view);
  delete view;
  releaseRender.release();
  QTest::qWait(150);
  EXPECT_TRUE(guard.isNull());
}

TEST(QuickViewMotion,
     PendingMarkdownAndArchiveWorkersAreSafeDuringRefreshAndTeardown) {
  MotionOverride motion(false);
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString markdown =
      writeText(dir, QStringLiteral("pending.md"),
                QByteArray(2 * 1024 * 1024, '#'));
  const QString archivedFile =
      writeText(dir, QStringLiteral("archived.txt"), QByteArrayLiteral("data"));
  const QString archive = dir.filePath(QStringLiteral("pending.zip"));
  QString archiveError;
  ASSERT_TRUE(ArchiveHandler::create(
      archive, {archivedFile}, QStringLiteral("zip"), &archiveError))
      << archiveError.toStdString();

  Settings markdownSettings(
      dir.filePath(QStringLiteral("markdown-settings.ini")));
  auto *markdownView = new QuickView(markdownSettings);
  markdownView->showFile(markdown);
  EXPECT_NO_THROW(markdownView->refreshPhosphor());
  QPointer<QuickView> markdownGuard(markdownView);
  delete markdownView;

  Settings archiveSettings(
      dir.filePath(QStringLiteral("archive-settings.ini")));
  auto *archiveView = new QuickView(archiveSettings);
  archiveView->showFile(archive);
  EXPECT_NO_THROW(archiveView->refreshPhosphor());
  QPointer<QuickView> archiveGuard(archiveView);
  delete archiveView;

  QTest::qWait(250);
  EXPECT_TRUE(markdownGuard.isNull());
  EXPECT_TRUE(archiveGuard.isNull());
}

TEST(QuickViewMotion,
     PendingOfficeDocumentAndSlidesWorkersAreSafeDuringTeardown) {
  MotionOverride motion(false);
  OfficeFixtureEnvironment office(400);
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString document = writeText(dir, QStringLiteral("pending.docx"),
                                     QByteArrayLiteral("fixture"));
  const QString slides = writeText(dir, QStringLiteral("pending.pptx"),
                                   QByteArrayLiteral("fixture"));

  Settings documentSettings(
      dir.filePath(QStringLiteral("document-settings.ini")));
  auto *documentView = new QuickView(documentSettings);
  QPointer<QuickView> documentGuard(documentView);
  {
    const QString marker = dir.filePath(QStringLiteral("document-started"));
    EnvironmentOverride markerEnvironment(
        "TTC_OFFICE_FIXTURE_MARKER", marker.toUtf8());
    documentView->showFile(document);
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(marker), 1000);
    EXPECT_NO_THROW(documentView->refreshPhosphor());
    delete documentView;
  }

  Settings slidesSettings(
      dir.filePath(QStringLiteral("slides-settings.ini")));
  auto *slidesView = new QuickView(slidesSettings);
  QPointer<QuickView> slidesGuard(slidesView);
  {
    const QString marker = dir.filePath(QStringLiteral("slides-started"));
    EnvironmentOverride markerEnvironment(
        "TTC_OFFICE_FIXTURE_MARKER", marker.toUtf8());
    slidesView->showFile(slides);
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(marker), 1000);
    EXPECT_NO_THROW(slidesView->refreshPhosphor());
    delete slidesView;
  }

  QTest::qWait(500);
  EXPECT_TRUE(documentGuard.isNull());
  EXPECT_TRUE(slidesGuard.isNull());
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
  QWidget *pdfPage = namedPage(view, "quickViewPdfPage");
  QWidget *pdfContent =
      scrollContent(pdfPage->findChild<QGraphicsView *>());
  expectFadeStarted(pdfContent);
  EXPECT_NO_THROW(view.refreshPhosphor());
  expectFadeStarted(pdfContent);
}

TEST(QuickViewMotion, PendingPdfWorkerIsSafeDuringRefreshAndTeardown) {
  MotionOverride motion(false);
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString pdfPath = dir.filePath(QStringLiteral("pending.pdf"));
  {
    QPdfWriter writer(pdfPath);
    writer.setResolution(72);
    QPainter painter(&writer);
    ASSERT_TRUE(painter.isActive());
    for (int page = 0; page < 40; ++page) {
      painter.drawText(QPoint(72, 72), QStringLiteral("pending %1").arg(page));
      if (page + 1 < 40)
        writer.newPage();
    }
  }

  Settings settings(dir.filePath(QStringLiteral("settings.ini")));
  auto *view = new QuickView(settings);
  view->showFile(pdfPath);
  EXPECT_NO_THROW(view->refreshPhosphor());
  QPointer<QuickView> guard(view);
  delete view;
  QTest::qWait(250);
  EXPECT_TRUE(guard.isNull());
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
  QWidget *content = scrollContent(view->findChild<QPlainTextEdit *>());
  ASSERT_NE(page, nullptr);
  ASSERT_NE(content, nullptr);

  view->revealStaticPage(page);
  expectFadeStarted(content);
  EXPECT_NO_THROW(view->refreshPhosphor());
  expectFadeStarted(content);

  QPointer<QuickView> guard(view);
  delete view;
  QTest::qWait(130);
  EXPECT_TRUE(guard.isNull());
}

} // namespace
