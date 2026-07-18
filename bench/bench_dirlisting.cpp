// Standalone benchmark for the spec target "open a 10,000-file directory
// in under 1 second". Measures FileSystemModel::setRootPath() end-to-end:
// background scan (QtConcurrent) + model reset on the calling thread.
//
// Usage: bench_dirlisting <directory>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <cstdio>

#include "FileSystemModel.h"

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);

    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <directory>\n", argv[0]);
        return 1;
    }

    FileSystemModel model;
    QEventLoop loop;
    int fileCount = 0;
    QObject::connect(&model, &FileSystemModel::loadFinished, &loop, [&](int count) {
        fileCount = count;
        loop.quit();
    });

    // Safety timeout so a hang doesn't block CI/terminal forever.
    QTimer::singleShot(30000, &loop, &QEventLoop::quit);

    QElapsedTimer timer;
    timer.start();
    model.setRootPath(QString::fromLocal8Bit(argv[1]));
    loop.exec();
    const qint64 elapsedMs = timer.elapsed();

    std::printf("Listed %d entries in %s: %lld ms\n", fileCount, argv[1],
                static_cast<long long>(elapsedMs));
    return 0;
}
