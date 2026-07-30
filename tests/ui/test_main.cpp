#include <gtest/gtest.h>

#include "ImagePreviewLoader.h"

#include <QApplication>
#include <QDir>
#include <QImage>
#include <QSemaphore>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include <memory>

namespace {

constexpr auto kImagePreviewShutdownProbe = "--image-preview-shutdown-probe";

int runImagePreviewShutdownProbe(QApplication &app) {
    struct Blockers {
        QSemaphore entered;
        QSemaphore release;
    };
    auto *blockers = new Blockers;

    QTemporaryDir dir;
    if (!dir.isValid())
        return 2;
    const QString path = QDir(dir.path()).filePath(QStringLiteral("shutdown.png"));
    QImage diskImage(QSize(80, 50), QImage::Format_ARGB32);
    diskImage.fill(Qt::red);
    if (!diskImage.save(path, "PNG"))
        return 3;

    auto loader = std::make_unique<ImagePreviewLoader>();
    loader->setWorkerCheckpointForTest(
        [blockers](ImagePreviewLoader::WorkerCheckpoint checkpoint, quint64) {
            if (checkpoint == ImagePreviewLoader::WorkerCheckpoint::LoadBeforeDecode ||
                checkpoint == ImagePreviewLoader::WorkerCheckpoint::RenderBeforeTransform ||
                checkpoint == ImagePreviewLoader::WorkerCheckpoint::RotationBeforeWrite) {
                blockers->entered.release();
                blockers->release.acquire();
            }
        });

    loader->load(path);
    if (!blockers->entered.tryAcquire(1, 5000))
        return 4;
    loader->render(diskImage, QSize(40, 25), QTransform());
    if (!blockers->entered.tryAcquire(1, 5000))
        return 5;
    loader->persistRotation(path, 90);
    if (!blockers->entered.tryAcquire(1, 5000))
        return 6;

    loader.reset();
    QTimer::singleShot(0, &app, &QCoreApplication::quit);
    return app.exec();
}

} // namespace

// The suite defaults to offscreen for headless CI. A caller can explicitly set
// QT_QPA_PLATFORM=xcb (with DISPLAY=:1) for real X11 geometry and theme tests.
int main(int argc, char **argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("FILECOMMANDER_DISABLE_ANIMATIONS", "1");
    // Every path QStandardPaths hands out moves to a throwaway test location for
    // the whole run. The thumbnail tests write to, count, and now DELETE files
    // in the cache directory; against the real one a test run would quietly
    // destroy the developer's own thumbnail cache (and fight any second process
    // sharing it). Set before the first test so it also covers the
    // ThumbnailCache singleton's construction, which touches the cache
    // directory on its own.
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    if (app.arguments().contains(QLatin1String(kImagePreviewShutdownProbe)))
        return runImagePreviewShutdownProbe(app);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
