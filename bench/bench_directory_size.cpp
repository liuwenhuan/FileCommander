#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTimer>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>

#include "DirectorySizeTask.h"
#include "LocalFileProvider.h"

namespace {

struct Measurement {
    qint64 wallMs = 0;
    qint64 p95EventLoopLatencyMs = 0;
};

bool writeFixture(const QString &path) {
    QDir root(path);
    if (!root.exists() && !QDir().mkpath(path))
        return false;
    for (int dirIndex = 0; dirIndex < 100; ++dirIndex) {
        const QString dirName = QStringLiteral("dir-%1").arg(dirIndex, 3, 10, QLatin1Char('0'));
        if (!root.exists(dirName) && !root.mkdir(dirName))
            return false;
        QDir child(root.filePath(dirName));
        for (int fileIndex = 0; fileIndex < 100; ++fileIndex) {
            const QString fileName =
                QStringLiteral("file-%1.bin").arg(fileIndex, 3, 10, QLatin1Char('0'));
            if (QFile::exists(child.filePath(fileName)))
                continue;
            QFile file(child.filePath(fileName));
            if (!file.open(QIODevice::WriteOnly) || file.write("x") != 1)
                return false;
        }
    }
    return true;
}

QStringList fixtureRoots(const QString &fixture) {
    QStringList roots;
    QDir dir(fixture);
    for (int i = 0; i < 100; ++i)
        roots.append(dir.filePath(QStringLiteral("dir-%1").arg(i, 3, 10, QLatin1Char('0'))));
    return roots;
}

qint64 percentile95(QVector<qint64> values) {
    if (values.isEmpty())
        return 0;
    std::sort(values.begin(), values.end());
    return values.at((values.size() - 1) * 95 / 100);
}

Measurement measure(const QStringList &roots, int workerCount) {
    const int taskCount = qMin(workerCount, roots.size());
    QVector<QStringList> batches(taskCount);
    for (int i = 0; i < roots.size(); ++i)
        batches[i % taskCount].append(roots.at(i));

    QEventLoop loop;
    QElapsedTimer elapsed;
    QVector<qint64> eventLoopLatencies;
    qint64 lastTick = 0;
    QTimer heartbeat;
    heartbeat.setInterval(1);
    QObject::connect(&heartbeat, &QTimer::timeout, [&] {
        const qint64 now = elapsed.elapsed();
        if (lastTick > 0)
            eventLoopLatencies.append(now - lastTick);
        lastTick = now;
    });

    const auto provider = std::shared_ptr<FileProvider>(LocalFileProvider::instance(),
                                                        [](FileProvider *) {});
    std::vector<std::unique_ptr<DirectorySizeTask>> tasks;
    int finishedCount = 0;
    for (int i = 0; i < taskCount; ++i) {
        auto task = std::make_unique<DirectorySizeTask>(i + 1, provider, batches.at(i));
        QObject::connect(task.get(), &DirectorySizeTask::finished, [&] {
            if (++finishedCount == taskCount)
                loop.quit();
        });
        tasks.push_back(std::move(task));
    }

    elapsed.start();
    heartbeat.start();
    for (const auto &task : tasks)
        task->start();
    loop.exec();
    heartbeat.stop();
    return {elapsed.elapsed(), percentile95(eventLoopLatencies)};
}

qint64 median(QVector<qint64> values) {
    std::sort(values.begin(), values.end());
    return values.at(values.size() / 2);
}

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);

    QString fixture;
    int iterations = 5;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == QStringLiteral("--fixture") && i + 1 < argc)
            fixture = QString::fromLocal8Bit(argv[++i]);
        else if (arg == QStringLiteral("--iterations") && i + 1 < argc)
            iterations = QString::fromLocal8Bit(argv[++i]).toInt();
    }
    if (fixture.isEmpty() || iterations <= 0) {
        std::fprintf(stderr, "usage: %s --fixture <path> --iterations <count>\n", argv[0]);
        return 1;
    }
    if (!writeFixture(fixture)) {
        std::fprintf(stderr, "could not create fixture at %s\n", qPrintable(fixture));
        return 1;
    }

    const QStringList roots = fixtureRoots(fixture);
    QVector<qint64> oneWorkerWall;
    QVector<qint64> oneWorkerP95;
    QVector<qint64> twoWorkerWall;
    QVector<qint64> twoWorkerP95;
    for (int i = 0; i < iterations; ++i) {
        const Measurement one = measure(roots, 1);
        const Measurement two = measure(roots, 2);
        oneWorkerWall.append(one.wallMs);
        oneWorkerP95.append(one.p95EventLoopLatencyMs);
        twoWorkerWall.append(two.wallMs);
        twoWorkerP95.append(two.p95EventLoopLatencyMs);
    }

    const qint64 oneMedian = median(oneWorkerWall);
    const qint64 twoMedian = median(twoWorkerWall);
    const qint64 oneP95 = median(oneWorkerP95);
    const qint64 twoP95 = median(twoWorkerP95);
    const bool twoWorkersAccepted =
        twoMedian * 100 <= oneMedian * 80 && twoP95 <= 16;
    std::printf("one worker: median wall %lld ms, median p95 GUI latency %lld ms\n",
                static_cast<long long>(oneMedian), static_cast<long long>(oneP95));
    std::printf("two workers: median wall %lld ms, median p95 GUI latency %lld ms\n",
                static_cast<long long>(twoMedian), static_cast<long long>(twoP95));
    std::printf("two local workers: %s\n", twoWorkersAccepted ? "accepted" : "rejected");
    return 0;
}
