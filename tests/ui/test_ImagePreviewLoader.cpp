#include "ImagePreviewLoader.h"

#include "QuickView.h"
#include "Settings.h"
#include "theme/Phosphor.h"

#include <gtest/gtest.h>

#include "TryUntil.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QMutex>
#include <QPointer>
#include <QProcess>
#include <QScrollArea>
#include <QSemaphore>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolBar>
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

QString writeDirectionalImage(const QDir &dir, const QString &name, const QSize &size) {
    QImage image(size, QImage::Format_ARGB32);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x)
            image.setPixelColor(x, y, x < image.width() / 2 ? Qt::red : Qt::green);
    }
    const QString path = dir.filePath(name);
    return image.save(path, "PNG") ? path : QString();
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

QAction *actionByText(QuickView &view, const QString &text) {
    if (auto *scroll =
            view.findChild<QScrollArea *>(QStringLiteral("imagePreviewScroll"))) {
        const auto toolbars = scroll->parentWidget()->findChildren<QToolBar *>();
        for (QToolBar *toolbar : toolbars) {
            for (QAction *action : toolbar->actions()) {
                if (action->text() == text)
                    return action;
            }
        }
    }
    const auto actions = view.findChildren<QAction *>();
    for (QAction *action : actions) {
        if (action->text() == text)
            return action;
    }
    return nullptr;
}

QCheckBox *checkBoxByText(QuickView &view, const QString &text) {
    const auto checkBoxes = view.findChildren<QCheckBox *>();
    for (QCheckBox *checkBox : checkBoxes) {
        if (checkBox->text() == text)
            return checkBox;
    }
    return nullptr;
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
    ASSERT_TRUE(loader.waitForIdleForTest(5000));
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
    ASSERT_TRUE(loader.waitForIdleForTest(5000));
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

TEST(ImagePreviewLoader, CoalescesPendingLoadsAndCancelsStaleWorkerBeforeDecode) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QDir root(dir.path());
    const QString firstPath =
        writeImage(root, QStringLiteral("first.png"), QSize(8, 8), Qt::red, "PNG");
    const QString middlePath =
        writeImage(root, QStringLiteral("middle.png"), QSize(8, 8), Qt::blue, "PNG");
    const QString latestPath =
        writeImage(root, QStringLiteral("latest.png"), QSize(8, 8), Qt::green, "PNG");
    ASSERT_FALSE(firstPath.isEmpty());
    ASSERT_FALSE(middlePath.isEmpty());
    ASSERT_FALSE(latestPath.isEmpty());

    ImagePreviewLoader loader;
    QSignalSpy loaded(&loader, &ImagePreviewLoader::loaded);
    QSemaphore firstEntered;
    QSemaphore releaseFirst;
    QMutex observedMutex;
    QVector<quint64> beforeDecode;
    QVector<quint64> afterDecode;
    bool heldFirst = false;
    loader.setWorkerCheckpointForTest(
        [&](ImagePreviewLoader::WorkerCheckpoint checkpoint, quint64 generation) {
            QMutexLocker locker(&observedMutex);
            if (checkpoint == ImagePreviewLoader::WorkerCheckpoint::LoadBeforeDecode) {
                beforeDecode.append(generation);
                if (!heldFirst) {
                    heldFirst = true;
                    firstEntered.release();
                    locker.unlock();
                    releaseFirst.acquire();
                }
            } else if (checkpoint == ImagePreviewLoader::WorkerCheckpoint::LoadAfterDecode) {
                afterDecode.append(generation);
            }
        });

    const quint64 first = loader.load(firstPath);
    ASSERT_TRUE(firstEntered.tryAcquire(1, 5000));
    const quint64 middle = loader.load(middlePath);
    const quint64 latest = loader.load(latestPath);
    releaseFirst.release();

    ASSERT_TRUE(loader.waitForIdleForTest(5000));
    QCoreApplication::processEvents();
    ASSERT_EQ(loaded.count(), 1);
    EXPECT_EQ(loaded.first().at(0).toULongLong(), latest);
    EXPECT_EQ(qvariant_cast<QImage>(loaded.first().at(1)).pixelColor(0, 0), QColor(Qt::green));

    QMutexLocker locker(&observedMutex);
    ASSERT_EQ(beforeDecode.size(), 2);
    EXPECT_EQ(beforeDecode.at(0), first);
    EXPECT_EQ(beforeDecode.at(1), latest);
    EXPECT_FALSE(beforeDecode.contains(middle));
    EXPECT_FALSE(afterDecode.contains(first));
}

TEST(ImagePreviewLoader, CoalescesPendingRendersAndCancelsStaleWorkerBeforeTransform) {
    QImage source(QSize(80, 50), QImage::Format_ARGB32);
    source.fill(Qt::cyan);

    ImagePreviewLoader loader;
    QSignalSpy rendered(&loader, &ImagePreviewLoader::rendered);
    QSemaphore firstEntered;
    QSemaphore releaseFirst;
    QMutex observedMutex;
    QVector<quint64> beforeTransform;
    QVector<quint64> afterTransform;
    bool heldFirst = false;
    loader.setWorkerCheckpointForTest(
        [&](ImagePreviewLoader::WorkerCheckpoint checkpoint, quint64 generation) {
            QMutexLocker locker(&observedMutex);
            if (checkpoint == ImagePreviewLoader::WorkerCheckpoint::RenderBeforeTransform) {
                beforeTransform.append(generation);
                if (!heldFirst) {
                    heldFirst = true;
                    firstEntered.release();
                    locker.unlock();
                    releaseFirst.acquire();
                }
            } else if (checkpoint ==
                       ImagePreviewLoader::WorkerCheckpoint::RenderAfterTransform) {
                afterTransform.append(generation);
            }
        });

    const quint64 first = loader.render(source, QSize(70, 40), QTransform());
    ASSERT_TRUE(firstEntered.tryAcquire(1, 5000));
    const quint64 middle = loader.render(source, QSize(40, 25), QTransform());
    const quint64 latest = loader.render(source, QSize(16, 10), QTransform());
    releaseFirst.release();

    ASSERT_TRUE(loader.waitForIdleForTest(5000));
    QCoreApplication::processEvents();
    ASSERT_EQ(rendered.count(), 1);
    EXPECT_EQ(rendered.first().at(0).toULongLong(), latest);
    EXPECT_EQ(qvariant_cast<QImage>(rendered.first().at(1)).size(), QSize(16, 10));

    QMutexLocker locker(&observedMutex);
    ASSERT_EQ(beforeTransform.size(), 2);
    EXPECT_EQ(beforeTransform.at(0), first);
    EXPECT_EQ(beforeTransform.at(1), latest);
    EXPECT_FALSE(beforeTransform.contains(middle));
    EXPECT_FALSE(afterTransform.contains(first));
}

TEST(ImagePreviewLoader, CancelledFileWaitReleasesDifferentPathAndSecondLoader) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QDir root(dir.path());
    const QString blockedPath =
        writeImage(root, QStringLiteral("blocked.png"), QSize(80, 50), Qt::red, "PNG");
    const QString switchedPath =
        writeImage(root, QStringLiteral("switched.png"), QSize(8, 6), Qt::green, "PNG");
    const QString otherPath =
        writeImage(root, QStringLiteral("other.png"), QSize(7, 5), Qt::blue, "PNG");
    ASSERT_FALSE(blockedPath.isEmpty());
    ASSERT_FALSE(switchedPath.isEmpty());
    ASSERT_FALSE(otherPath.isEmpty());

    ImagePreviewLoader firstLoader;
    ImagePreviewLoader secondLoader;
    QSignalSpy firstLoaded(&firstLoader, &ImagePreviewLoader::loaded);
    QSignalSpy secondLoaded(&secondLoader, &ImagePreviewLoader::loaded);
    QSemaphore writerEntered;
    QSemaphore releaseWriter;
    QSemaphore readerWaiting;
    firstLoader.setWorkerCheckpointForTest(
        [&](ImagePreviewLoader::WorkerCheckpoint checkpoint, quint64) {
            if (checkpoint == ImagePreviewLoader::WorkerCheckpoint::RotationBeforeWrite) {
                writerEntered.release();
                releaseWriter.acquire();
            } else if (checkpoint ==
                       ImagePreviewLoader::WorkerCheckpoint::LoadWaitingForFile) {
                readerWaiting.release();
            }
        });

    firstLoader.persistRotation(blockedPath, 90);
    ASSERT_TRUE(writerEntered.tryAcquire(1, 5000));
    struct ReleaseOnExit {
        QSemaphore &semaphore;
        bool armed = true;
        ~ReleaseOnExit() {
            if (armed)
                semaphore.release();
        }
    } releaseOnExit{releaseWriter};

    firstLoader.load(blockedPath);
    ASSERT_TRUE(readerWaiting.tryAcquire(1, 5000));
    const quint64 switchedGeneration = firstLoader.load(switchedPath);
    const quint64 otherGeneration = secondLoader.load(otherPath);

    FC_TRY_COMPARE_WITH_TIMEOUT(firstLoaded.count(), 1, 1000);
    FC_TRY_COMPARE_WITH_TIMEOUT(secondLoaded.count(), 1, 1000);
    EXPECT_EQ(firstLoaded.first().at(0).toULongLong(), switchedGeneration);
    EXPECT_EQ(secondLoaded.first().at(0).toULongLong(), otherGeneration);
    EXPECT_EQ(qvariant_cast<QImage>(firstLoaded.first().at(1)).size(), QSize(8, 6));
    EXPECT_EQ(qvariant_cast<QImage>(secondLoaded.first().at(1)).size(), QSize(7, 5));

    releaseWriter.release();
    releaseOnExit.armed = false;
    EXPECT_TRUE(firstLoader.waitForIdleForTest(5000));
    EXPECT_TRUE(secondLoader.waitForIdleForTest(5000));
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

    FC_TRY_VERIFY_WITH_TIMEOUT(previewInfoText(view).contains(QStringLiteral("No preview available")),
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

    auto *loader = view.findChild<ImagePreviewLoader *>();
    QLabel *label = imageLabel(view);
    ASSERT_NE(loader, nullptr);
    ASSERT_NE(label, nullptr);
    FC_TRY_VERIFY_WITH_TIMEOUT(label->pixmap() && !label->pixmap()->isNull(), 5000);
    ASSERT_TRUE(loader->waitForIdleForTest(5000));
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

    auto *loader = view.findChild<ImagePreviewLoader *>();
    QLabel *label = imageLabel(view);
    ASSERT_NE(loader, nullptr);
    ASSERT_NE(label, nullptr);
    FC_TRY_VERIFY_WITH_TIMEOUT(label->pixmap() && !label->pixmap()->isNull(), 5000);
    ASSERT_TRUE(loader->waitForIdleForTest(5000));
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
    FC_TRY_VERIFY_WITH_TIMEOUT(label->pixmap() && label->pixmap()->size() != before, 5000);
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

    ASSERT_TRUE(ImagePreviewLoader::waitForAllForTest(15000));
    QCoreApplication::processEvents();
    SUCCEED();
}

TEST(ImagePreviewLoader, RotateThenSamePathReloadWaitsForPersistedImage) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QDir root(dir.path());
    const QString path =
        writeImage(root, QStringLiteral("rotate.png"), QSize(160, 100), Qt::magenta, "PNG");
    ASSERT_FALSE(path.isEmpty());

    Settings settings(root.filePath(QStringLiteral("settings.ini")));
    QuickView view(settings);
    view.resize(640, 480);
    view.show();
    view.showFile(path);

    auto *loader = view.findChild<ImagePreviewLoader *>();
    QLabel *label = imageLabel(view);
    ASSERT_NE(loader, nullptr);
    ASSERT_NE(label, nullptr);
    FC_TRY_VERIFY_WITH_TIMEOUT(label->pixmap() && !label->pixmap()->isNull(), 5000);

    QSemaphore writeReserved;
    QSemaphore releaseWrite;
    loader->setWorkerCheckpointForTest(
        [&](ImagePreviewLoader::WorkerCheckpoint checkpoint, quint64) {
            if (checkpoint == ImagePreviewLoader::WorkerCheckpoint::RotationBeforeWrite) {
                writeReserved.release();
                releaseWrite.acquire();
            }
        });
    QAction *rotateRight = actionByText(view, QStringLiteral("Rotate Right"));
    ASSERT_NE(rotateRight, nullptr);
    rotateRight->trigger();
    ASSERT_TRUE(writeReserved.tryAcquire(1, 5000));

    view.showFile(path);
    QTest::qWait(20);
    EXPECT_FALSE(label->pixmap());
    releaseWrite.release();

    ASSERT_TRUE(loader->waitForIdleForTest(5000));
    ASSERT_NE(label->pixmap(), nullptr);
    EXPECT_LT(label->pixmap()->width(), label->pixmap()->height());
    const QImage persisted(path);
    ASSERT_FALSE(persisted.isNull());
    EXPECT_EQ(persisted.size(), QSize(100, 160));
}

TEST(ImagePreviewLoader, QuickViewDisplaysOnlyLatestRenderRequest) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QDir root(dir.path());
    const QString path =
        writeImage(root, QStringLiteral("latest.png"), QSize(160, 100), Qt::blue, "PNG");
    ASSERT_FALSE(path.isEmpty());

    Settings settings(root.filePath(QStringLiteral("settings.ini")));
    QuickView view(settings);
    view.resize(640, 480);
    view.show();
    view.showFile(path);
    auto *loader = view.findChild<ImagePreviewLoader *>();
    QLabel *label = imageLabel(view);
    ASSERT_NE(loader, nullptr);
    ASSERT_NE(label, nullptr);
    ASSERT_TRUE(loader->waitForIdleForTest(5000));
    ASSERT_NE(label->pixmap(), nullptr);
    const QSize initialSize = label->pixmap()->size();

    QSemaphore firstRenderEntered;
    QSemaphore releaseFirstRender;
    bool heldFirst = false;
    loader->setWorkerCheckpointForTest(
        [&](ImagePreviewLoader::WorkerCheckpoint checkpoint, quint64) {
            if (checkpoint == ImagePreviewLoader::WorkerCheckpoint::RenderBeforeTransform &&
                !heldFirst) {
                heldFirst = true;
                firstRenderEntered.release();
                releaseFirstRender.acquire();
            }
        });
    QAction *zoomIn = actionByText(view, QStringLiteral("Zoom In"));
    ASSERT_NE(zoomIn, nullptr);
    zoomIn->trigger();
    ASSERT_TRUE(firstRenderEntered.tryAcquire(1, 5000));
    zoomIn->trigger();
    EXPECT_EQ(label->pixmap()->size(), initialSize);
    releaseFirstRender.release();

    ASSERT_TRUE(loader->waitForIdleForTest(5000));
    ASSERT_NE(label->pixmap(), nullptr);
    EXPECT_GT(label->pixmap()->width(), qRound(initialSize.width() * 1.4));
}

TEST(ImagePreviewLoader, PageChangeInvalidatesInFlightRender) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QDir root(dir.path());
    const QString imagePath =
        writeImage(root, QStringLiteral("page.png"), QSize(160, 100), Qt::blue, "PNG");
    const QString textPath = root.filePath(QStringLiteral("page.txt"));
    QFile textFile(textPath);
    ASSERT_TRUE(textFile.open(QIODevice::WriteOnly));
    ASSERT_EQ(textFile.write("new page"), 8);
    textFile.close();

    Settings settings(root.filePath(QStringLiteral("settings.ini")));
    QuickView view(settings);
    view.resize(640, 480);
    view.show();
    view.showFile(imagePath);
    auto *loader = view.findChild<ImagePreviewLoader *>();
    QLabel *label = imageLabel(view);
    ASSERT_NE(loader, nullptr);
    ASSERT_NE(label, nullptr);
    ASSERT_TRUE(loader->waitForIdleForTest(5000));
    ASSERT_NE(label->pixmap(), nullptr);
    const QSize displayedBeforePageChange = label->pixmap()->size();

    QSemaphore renderEntered;
    QSemaphore releaseRender;
    loader->setWorkerCheckpointForTest(
        [&](ImagePreviewLoader::WorkerCheckpoint checkpoint, quint64) {
            if (checkpoint == ImagePreviewLoader::WorkerCheckpoint::RenderBeforeTransform) {
                renderEntered.release();
                releaseRender.acquire();
            }
        });
    QAction *zoomIn = actionByText(view, QStringLiteral("Zoom In"));
    ASSERT_NE(zoomIn, nullptr);
    zoomIn->trigger();
    ASSERT_TRUE(renderEntered.tryAcquire(1, 5000));
    view.showFile(textPath);
    releaseRender.release();

    ASSERT_TRUE(loader->waitForIdleForTest(5000));
    auto *stack = view.findChild<QStackedWidget *>();
    ASSERT_NE(stack, nullptr);
    EXPECT_NE(stack->currentWidget(), label->parentWidget());
    ASSERT_NE(label->pixmap(), nullptr);
    EXPECT_EQ(label->pixmap()->size(), displayedBeforePageChange);
}

TEST(ImagePreviewLoader, DestroyingQuickViewDuringRenderDoesNotWaitOrDeliver) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QDir root(dir.path());
    const QString path =
        writeImage(root, QStringLiteral("teardown.png"), QSize(160, 100), Qt::blue, "PNG");
    ASSERT_FALSE(path.isEmpty());

    Settings settings(root.filePath(QStringLiteral("settings.ini")));
    auto view = std::make_unique<QuickView>(settings);
    view->resize(640, 480);
    view->show();
    view->showFile(path);
    auto *loader = view->findChild<ImagePreviewLoader *>();
    ASSERT_NE(loader, nullptr);
    ASSERT_TRUE(loader->waitForIdleForTest(5000));

    QSemaphore renderEntered;
    QSemaphore releaseRender;
    loader->setWorkerCheckpointForTest(
        [&](ImagePreviewLoader::WorkerCheckpoint checkpoint, quint64) {
            if (checkpoint == ImagePreviewLoader::WorkerCheckpoint::RenderBeforeTransform) {
                renderEntered.release();
                releaseRender.acquire();
            }
        });
    QAction *zoomIn = actionByText(*view, QStringLiteral("Zoom In"));
    ASSERT_NE(zoomIn, nullptr);
    zoomIn->trigger();
    ASSERT_TRUE(renderEntered.tryAcquire(1, 5000));

    QPointer<QuickView> guard(view.get());
    view.reset();
    EXPECT_TRUE(guard.isNull());
    releaseRender.release();
    EXPECT_TRUE(ImagePreviewLoader::waitForAllForTest(5000));
    QCoreApplication::processEvents();
}

TEST(ImagePreviewLoader, ApplicationShutdownDoesNotDestroyActiveWorkerPools) {
    QProcess probe;
    probe.start(QCoreApplication::applicationFilePath(),
                {QStringLiteral("--image-preview-shutdown-probe")});
    ASSERT_TRUE(probe.waitForStarted(5000));
    const bool exited = probe.waitForFinished(2000);
    if (!exited) {
        probe.kill();
        probe.waitForFinished(5000);
    }
    ASSERT_TRUE(exited) << probe.readAllStandardError().constData();
    EXPECT_EQ(probe.exitStatus(), QProcess::NormalExit);
    EXPECT_EQ(probe.exitCode(), 0) << probe.readAllStandardError().constData();
}

TEST(ImagePreviewLoader, FitModeRecomputesAfterQuarterTurn) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QDir root(dir.path());
    const QString path =
        writeImage(root, QStringLiteral("fit.png"), QSize(320, 100), Qt::yellow, "PNG");
    ASSERT_FALSE(path.isEmpty());

    Settings settings(root.filePath(QStringLiteral("settings.ini")));
    QuickView view(settings);
    view.resize(640, 480);
    view.show();
    view.showFile(path);
    auto *loader = view.findChild<ImagePreviewLoader *>();
    QLabel *label = imageLabel(view);
    auto *scroll = view.findChild<QScrollArea *>(QStringLiteral("imagePreviewScroll"));
    ASSERT_NE(loader, nullptr);
    ASSERT_NE(label, nullptr);
    ASSERT_NE(scroll, nullptr);
    ASSERT_TRUE(loader->waitForIdleForTest(5000));

    QAction *rotateRight = actionByText(view, QStringLiteral("Rotate Right"));
    ASSERT_NE(rotateRight, nullptr);
    rotateRight->trigger();
    ASSERT_TRUE(loader->waitForIdleForTest(5000));

    ASSERT_NE(label->pixmap(), nullptr);
    EXPECT_LT(label->pixmap()->width(), label->pixmap()->height());
    EXPECT_LE(label->pixmap()->width(), scroll->viewport()->width());
    EXPECT_LE(label->pixmap()->height(), scroll->viewport()->height());
}

TEST(ImagePreviewLoader, LockedZoomCarriesScaleAcrossImages) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QDir root(dir.path());
    const QString large =
        writeImage(root, QStringLiteral("large.png"), QSize(160, 100), Qt::red, "PNG");
    const QString small =
        writeImage(root, QStringLiteral("small.png"), QSize(80, 50), Qt::green, "PNG");
    ASSERT_FALSE(large.isEmpty());
    ASSERT_FALSE(small.isEmpty());

    Settings settings(root.filePath(QStringLiteral("settings.ini")));
    QuickView view(settings);
    view.resize(640, 480);
    view.show();
    view.showFile(large);
    auto *loader = view.findChild<ImagePreviewLoader *>();
    QLabel *label = imageLabel(view);
    ASSERT_NE(loader, nullptr);
    ASSERT_NE(label, nullptr);
    ASSERT_TRUE(loader->waitForIdleForTest(5000));

    QAction *zoomIn = actionByText(view, QStringLiteral("Zoom In"));
    QCheckBox *lockZoom = checkBoxByText(view, QStringLiteral("Lock Zoom"));
    ASSERT_NE(zoomIn, nullptr);
    ASSERT_NE(lockZoom, nullptr);
    zoomIn->trigger();
    ASSERT_TRUE(loader->waitForIdleForTest(5000));
    const QSize largeDisplay = label->pixmap()->size();
    lockZoom->setChecked(true);

    view.showFile(small);
    ASSERT_TRUE(loader->waitForIdleForTest(5000));
    ASSERT_NE(label->pixmap(), nullptr);
    EXPECT_NEAR(label->pixmap()->width(), largeDisplay.width() / 2.0, 2.0);
    EXPECT_NEAR(label->pixmap()->height(), largeDisplay.height() / 2.0, 2.0);
}

TEST(ImagePreviewLoader, RepeatedRotationIsCumulativeOnScreenAndDisk) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QDir root(dir.path());
    const QString path =
        writeDirectionalImage(root, QStringLiteral("repeat.png"), QSize(160, 100));
    ASSERT_FALSE(path.isEmpty());

    Settings settings(root.filePath(QStringLiteral("settings.ini")));
    QuickView view(settings);
    view.resize(640, 480);
    view.show();
    view.showFile(path);
    auto *loader = view.findChild<ImagePreviewLoader *>();
    QLabel *label = imageLabel(view);
    ASSERT_NE(loader, nullptr);
    ASSERT_NE(label, nullptr);
    ASSERT_TRUE(loader->waitForIdleForTest(5000));

    QAction *rotateRight = actionByText(view, QStringLiteral("Rotate Right"));
    ASSERT_NE(rotateRight, nullptr);
    rotateRight->trigger();
    rotateRight->trigger();
    ASSERT_TRUE(loader->waitForIdleForTest(5000));

    ASSERT_NE(label->pixmap(), nullptr);
    EXPECT_GT(label->pixmap()->width(), label->pixmap()->height());
    const QImage persisted(path);
    ASSERT_EQ(persisted.size(), QSize(160, 100));
    EXPECT_EQ(persisted.pixelColor(10, 50), QColor(Qt::green));
    EXPECT_EQ(persisted.pixelColor(150, 50), QColor(Qt::red));
}

TEST(ImagePreviewLoader, ThemeRefreshSupersedesPendingRender) {
    struct TintReset {
        ~TintReset() { fc::setPreviewTint(QColor()); }
    } tintReset;
    fc::setPreviewTint(QColor());

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QDir root(dir.path());
    const QString path =
        writeImage(root, QStringLiteral("theme.png"), QSize(160, 100), Qt::white, "PNG");
    ASSERT_FALSE(path.isEmpty());

    Settings settings(root.filePath(QStringLiteral("settings.ini")));
    QuickView view(settings);
    view.resize(640, 480);
    view.show();
    view.showFile(path);
    auto *loader = view.findChild<ImagePreviewLoader *>();
    QLabel *label = imageLabel(view);
    ASSERT_NE(loader, nullptr);
    ASSERT_NE(label, nullptr);
    ASSERT_TRUE(loader->waitForIdleForTest(5000));

    QSemaphore renderEntered;
    QSemaphore releaseRender;
    bool heldFirst = false;
    loader->setWorkerCheckpointForTest(
        [&](ImagePreviewLoader::WorkerCheckpoint checkpoint, quint64) {
            if (checkpoint == ImagePreviewLoader::WorkerCheckpoint::RenderBeforeTransform &&
                !heldFirst) {
                heldFirst = true;
                renderEntered.release();
                releaseRender.acquire();
            }
        });
    QAction *zoomIn = actionByText(view, QStringLiteral("Zoom In"));
    ASSERT_NE(zoomIn, nullptr);
    zoomIn->trigger();
    ASSERT_TRUE(renderEntered.tryAcquire(1, 5000));
    fc::setPreviewTint(QColor(0x33, 0xff, 0x88));
    view.refreshPhosphor();
    releaseRender.release();

    ASSERT_TRUE(loader->waitForIdleForTest(5000));
    ASSERT_NE(label->pixmap(), nullptr);
    const QImage displayed = label->pixmap()->toImage();
    const QColor center = displayed.pixelColor(displayed.width() / 2, displayed.height() / 2);
    EXPECT_GT(center.green(), center.red());
    EXPECT_GT(center.green(), center.blue());
}

// The FIRST image after launch was not fitted to the pane; every later one was.
//
// A QStackedLayout lays out only its current page, so until the image page is
// revealed its scroll viewport is still Qt's default ~100x30 -- and that is
// what fitScale() measured, at the moment the image finished loading. The page
// becomes current afterwards, and nothing recomputed the fit against the real
// size. From the second image on the page is already current, which is exactly
// why only the first looked wrong.
//
// Asserted with "fills one axis", not "does not overflow": a scale of 0.05
// satisfies the latter perfectly, which is how the existing rotate test passed
// straight through this.
TEST(ImagePreviewLoader, TheFirstImageAfterLaunchIsFittedToThePane) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QDir root(dir.path());
    // Wider than tall, and bigger than the pane, so fitting is width-bound and
    // the result is unambiguous.
    const QString path =
        writeImage(root, QStringLiteral("first.png"), QSize(1600, 400), Qt::cyan, "PNG");
    ASSERT_FALSE(path.isEmpty());

    Settings settings(root.filePath(QStringLiteral("settings.ini")));
    QuickView view(settings);
    view.resize(640, 480);
    view.show();
    QTest::qWaitForWindowExposed(&view);

    // The very first showFile() of this QuickView -- no earlier image, no
    // rotate, nothing that would have laid the page out already.
    view.showFile(path);
    auto *loader = view.findChild<ImagePreviewLoader *>();
    ASSERT_NE(loader, nullptr);
    ASSERT_TRUE(loader->waitForIdleForTest(5000));

    QLabel *label = imageLabel(view);
    auto *scroll = view.findChild<QScrollArea *>(QStringLiteral("imagePreviewScroll"));
    ASSERT_NE(label, nullptr);
    ASSERT_NE(scroll, nullptr);

    // The refit is debounced; give it its timer plus the re-render.
    QTest::qWait(200);
    ASSERT_TRUE(loader->waitForIdleForTest(5000));
    ASSERT_NE(label->pixmap(), nullptr);

    const int viewportWidth = scroll->viewport()->width();
    ASSERT_GT(viewportWidth, 200) << "the pane itself never got laid out";
    EXPECT_LE(label->pixmap()->width(), viewportWidth);
    EXPECT_GE(label->pixmap()->width(), viewportWidth - 24)
        << "first image is " << label->pixmap()->width() << "px wide in a "
        << viewportWidth << "px viewport -- it was fitted to something else";
}
