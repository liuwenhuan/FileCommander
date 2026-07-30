#include "ImagePreviewLoader.h"

#include "QuickView.h"
#include "Settings.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QPointer>
#include <QScrollArea>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>
#include <QTimer>
#include <QWheelEvent>

#include <memory>

namespace {

QString writeImage(const QDir &dir, const QString &name, const QSize &size, const QColor &color,
                   const char *format) {
    QImage image(size, QImage::Format_ARGB32);
    image.fill(color);
    const QString path = dir.filePath(name);
    return image.save(path, format) ? path : QString();
}

void drainPreviewWorkers() {
    ASSERT_TRUE(QThreadPool::globalInstance()->waitForDone(15000));
    QCoreApplication::processEvents();
}

QLabel *imageLabel(QuickView &view) {
    auto *scroll = view.findChild<QScrollArea *>(QStringLiteral("imagePreviewScroll"));
    return scroll ? qobject_cast<QLabel *>(scroll->widget()) : nullptr;
}

QString previewInfoText(QuickView &view) {
    auto *stack = view.findChild<QStackedWidget *>();
    if (!stack)
        return {};
    auto *label = qobject_cast<QLabel *>(stack->currentWidget());
    return label ? label->text() : QString();
}

} // namespace

TEST(ImagePreviewLoader, DecodesPixelsAndMetadata) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeImage(QDir(dir.path()), QStringLiteral("alpha.png"), QSize(17, 11),
                                    QColor(20, 80, 140, 120), "PNG");
    ASSERT_FALSE(path.isEmpty());

    ImagePreviewLoader loader;
    QSignalSpy loaded(&loader, &ImagePreviewLoader::loaded);

    const quint64 generation = loader.load(path);

    ASSERT_TRUE(loaded.wait(5000));
    ASSERT_EQ(loaded.count(), 1);
    const QList<QVariant> result = loaded.takeFirst();
    EXPECT_EQ(result.at(0).toULongLong(), generation);
    const QImage image = qvariant_cast<QImage>(result.at(1));
    const ImageMetadata metadata = qvariant_cast<ImageMetadata>(result.at(2));
    EXPECT_TRUE(result.at(3).toString().isEmpty());
    ASSERT_FALSE(image.isNull());
    EXPECT_EQ(image.size(), QSize(17, 11));
    EXPECT_EQ(image.pixelColor(8, 5), QColor(20, 80, 140, 120));
    EXPECT_EQ(metadata.format, QStringLiteral("PNG"));
    EXPECT_EQ(metadata.dimensions, QSize(17, 11));
    EXPECT_EQ(metadata.depth, image.depth());
}

TEST(ImagePreviewLoader, NewLoadSuppressesSlowerGeneration) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QDir root(dir.path());
    const QString slow =
        writeImage(root, QStringLiteral("slow.bmp"), QSize(3200, 2400), Qt::red, "BMP");
    const QString fast =
        writeImage(root, QStringLiteral("fast.png"), QSize(2, 2), Qt::green, "PNG");
    ASSERT_FALSE(slow.isEmpty());
    ASSERT_FALSE(fast.isEmpty());

    ImagePreviewLoader loader;
    QSignalSpy loaded(&loader, &ImagePreviewLoader::loaded);

    const quint64 slowGeneration = loader.load(slow);
    const quint64 fastGeneration = loader.load(fast);
    EXPECT_GT(fastGeneration, slowGeneration);

    ASSERT_TRUE(loaded.wait(5000));
    drainPreviewWorkers();
    ASSERT_EQ(loaded.count(), 1);
    const QList<QVariant> result = loaded.takeFirst();
    EXPECT_EQ(result.at(0).toULongLong(), fastGeneration);
    const QImage image = qvariant_cast<QImage>(result.at(1));
    ASSERT_EQ(image.size(), QSize(2, 2));
    EXPECT_EQ(image.pixelColor(0, 0), QColor(Qt::green));
}

TEST(ImagePreviewLoader, NewestRenderIsTheOnlyAcceptedResult) {
    QImage source(QSize(1800, 1200), QImage::Format_ARGB32);
    source.fill(QColor(30, 90, 170, 200));

    ImagePreviewLoader loader;
    QSignalSpy rendered(&loader, &ImagePreviewLoader::rendered);
    QTransform quarterTurn;
    quarterTurn.rotate(90);

    const quint64 first = loader.render(source, QSize(900, 600), QTransform());
    const quint64 second = loader.render(source, QSize(450, 300), QTransform());
    const quint64 third = loader.render(source, QSize(80, 120), quarterTurn);
    EXPECT_LT(first, second);
    EXPECT_LT(second, third);

    ASSERT_TRUE(rendered.wait(5000));
    drainPreviewWorkers();
    ASSERT_EQ(rendered.count(), 1);
    const QList<QVariant> result = rendered.takeFirst();
    EXPECT_EQ(result.at(0).toULongLong(), third);
    const QImage image = qvariant_cast<QImage>(result.at(1));
    ASSERT_EQ(image.size(), QSize(80, 120));
    const QColor center = image.pixelColor(40, 60);
    EXPECT_NEAR(center.red(), 30, 1);
    EXPECT_NEAR(center.green(), 90, 1);
    EXPECT_NEAR(center.blue(), 170, 1);
    EXPECT_EQ(center.alpha(), 200);
}

TEST(ImagePreviewLoader, DamagedImageKeepsQuickViewOnNoPreviewPage) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("damaged.png"));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write("not a png"), 9);
    file.close();

    Settings settings(QDir(dir.path()).filePath(QStringLiteral("settings.ini")));
    QuickView view(settings);
    view.showFile(path);

    QTRY_VERIFY_WITH_TIMEOUT(previewInfoText(view).contains(QStringLiteral("No preview available")),
                             5000);
}

TEST(ImagePreviewLoader, QuickViewRejectsLateLoadAfterNewFile) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QDir root(dir.path());
    const QString slow =
        writeImage(root, QStringLiteral("slow.bmp"), QSize(3200, 2400), Qt::red, "BMP");
    const QString fast =
        writeImage(root, QStringLiteral("fast.png"), QSize(64, 48), Qt::green, "PNG");
    ASSERT_FALSE(slow.isEmpty());
    ASSERT_FALSE(fast.isEmpty());

    Settings settings(root.filePath(QStringLiteral("settings.ini")));
    QuickView view(settings);
    view.resize(640, 480);
    view.show();
    view.showFile(slow);
    view.showFile(fast);

    QLabel *label = imageLabel(view);
    ASSERT_NE(label, nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(label->pixmap() && !label->pixmap()->isNull(), 5000);
    drainPreviewWorkers();
    ASSERT_NE(label->pixmap(), nullptr);
    const QImage displayed = label->pixmap()->toImage();
    const QColor center = displayed.pixelColor(displayed.width() / 2, displayed.height() / 2);
    EXPECT_GT(center.green(), center.red());
}

TEST(ImagePreviewLoader, WheelDebouncesForFiftyMillisecondsAndKeepsDisplayedPixmap) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QDir root(dir.path());
    const QString path =
        writeImage(root, QStringLiteral("zoom.png"), QSize(160, 100), Qt::blue, "PNG");
    ASSERT_FALSE(path.isEmpty());

    Settings settings(root.filePath(QStringLiteral("settings.ini")));
    QuickView view(settings);
    view.resize(640, 480);
    view.show();
    view.showFile(path);

    QLabel *label = imageLabel(view);
    ASSERT_NE(label, nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(label->pixmap() && !label->pixmap()->isNull(), 5000);
    drainPreviewWorkers();
    const QSize before = label->pixmap()->size();

    auto *timer = view.findChild<QTimer *>(QStringLiteral("imageWheelRenderTimer"));
    ASSERT_NE(timer, nullptr);
    EXPECT_TRUE(timer->isSingleShot());
    EXPECT_EQ(timer->interval(), 50);

    auto *scroll = view.findChild<QScrollArea *>(QStringLiteral("imagePreviewScroll"));
    ASSERT_NE(scroll, nullptr);
    QWheelEvent wheel(scroll->viewport()->rect().center(),
                      scroll->viewport()->mapToGlobal(scroll->viewport()->rect().center()),
                      QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase,
                      false);
    QApplication::sendEvent(scroll->viewport(), &wheel);

    EXPECT_TRUE(timer->isActive());
    ASSERT_NE(label->pixmap(), nullptr);
    EXPECT_EQ(label->pixmap()->size(), before);
    QTest::qWait(30);
    ASSERT_NE(label->pixmap(), nullptr);
    EXPECT_EQ(label->pixmap()->size(), before);
    QTRY_VERIFY_WITH_TIMEOUT(label->pixmap() && label->pixmap()->size() != before, 5000);
}

TEST(ImagePreviewLoader, DestroyingQuickViewInvalidatesInFlightWork) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QDir root(dir.path());
    const QString slow =
        writeImage(root, QStringLiteral("slow.bmp"), QSize(3200, 2400), Qt::red, "BMP");
    ASSERT_FALSE(slow.isEmpty());

    Settings settings(root.filePath(QStringLiteral("settings.ini")));
    auto view = std::make_unique<QuickView>(settings);
    QPointer<QuickView> guard(view.get());
    view->showFile(slow);
    view.reset();
    EXPECT_TRUE(guard.isNull());

    drainPreviewWorkers();
    SUCCEED();
}
