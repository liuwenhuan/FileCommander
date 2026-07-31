#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMediaContent>
#include <QMediaPlayer>
#include <QThread>
#include <QUrl>
#include <QVideoWidget>

#include <functional>

namespace {

struct ProbeOptions {
    QString fixtures;
    QString jsonPath;
    QString uncRoot;
};

struct ProbeCase {
    QString name;
    bool mandatory = true;
    bool passed = false;
    QString error;
    qint64 loadMs = -1;
    qint64 firstFrameMs = -1;
    qint64 durationMs = -1;
    qint64 seekDeltaMs = -1;
    QJsonArray events;
};

QString mediaStatusName(QMediaPlayer::MediaStatus status) {
    switch (status) {
    case QMediaPlayer::UnknownMediaStatus:
        return QStringLiteral("UnknownMediaStatus");
    case QMediaPlayer::NoMedia:
        return QStringLiteral("NoMedia");
    case QMediaPlayer::LoadingMedia:
        return QStringLiteral("LoadingMedia");
    case QMediaPlayer::LoadedMedia:
        return QStringLiteral("LoadedMedia");
    case QMediaPlayer::StalledMedia:
        return QStringLiteral("StalledMedia");
    case QMediaPlayer::BufferingMedia:
        return QStringLiteral("BufferingMedia");
    case QMediaPlayer::BufferedMedia:
        return QStringLiteral("BufferedMedia");
    case QMediaPlayer::EndOfMedia:
        return QStringLiteral("EndOfMedia");
    case QMediaPlayer::InvalidMedia:
        return QStringLiteral("InvalidMedia");
    }
    return QStringLiteral("MediaStatus(%1)").arg(static_cast<int>(status));
}

QString stateName(QMediaPlayer::State state) {
    switch (state) {
    case QMediaPlayer::StoppedState:
        return QStringLiteral("StoppedState");
    case QMediaPlayer::PlayingState:
        return QStringLiteral("PlayingState");
    case QMediaPlayer::PausedState:
        return QStringLiteral("PausedState");
    }
    return QStringLiteral("State(%1)").arg(static_cast<int>(state));
}

QString errorName(QMediaPlayer::Error error) {
    switch (error) {
    case QMediaPlayer::NoError:
        return QStringLiteral("NoError");
    case QMediaPlayer::ResourceError:
        return QStringLiteral("ResourceError");
    case QMediaPlayer::FormatError:
        return QStringLiteral("FormatError");
    case QMediaPlayer::NetworkError:
        return QStringLiteral("NetworkError");
    case QMediaPlayer::AccessDeniedError:
        return QStringLiteral("AccessDeniedError");
    case QMediaPlayer::ServiceMissingError:
        return QStringLiteral("ServiceMissingError");
    }
    return QStringLiteral("Error(%1)").arg(static_cast<int>(error));
}

void appendEvent(ProbeCase *result, const QElapsedTimer &elapsed, const QString &name,
                 const QString &value = QString()) {
    QJsonObject event;
    event.insert(QStringLiteral("ms"), elapsed.isValid() ? elapsed.elapsed() : -1);
    event.insert(QStringLiteral("name"), name);
    if (!value.isEmpty())
        event.insert(QStringLiteral("value"), value);
    result->events.append(event);
}

bool waitUntil(int timeoutMs, const std::function<bool()> &ready) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < timeoutMs) {
        if (ready())
            return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        QThread::msleep(10);
    }
    return ready();
}

bool imageHasVisiblePixels(const QImage &image) {
    if (image.isNull())
        return false;
    const QRgb first = image.pixel(0, 0);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixel(x, y) != first)
                return true;
        }
    }
    return qAlpha(first) > 0 && qGray(first) > 8;
}

QJsonObject toJson(const ProbeCase &result) {
    QJsonObject object;
    object.insert(QStringLiteral("name"), result.name);
    object.insert(QStringLiteral("mandatory"), result.mandatory);
    object.insert(QStringLiteral("passed"), result.passed);
    object.insert(QStringLiteral("error"), result.error);
    object.insert(QStringLiteral("loadMs"), result.loadMs);
    object.insert(QStringLiteral("firstFrameMs"), result.firstFrameMs);
    object.insert(QStringLiteral("durationMs"), result.durationMs);
    object.insert(QStringLiteral("seekDeltaMs"), result.seekDeltaMs);
    object.insert(QStringLiteral("events"), result.events);
    return object;
}

ProbeCase runPlaybackCase(const QString &name, const QMediaContent &content, bool video,
                          bool mandatory, QIODevice *device = nullptr) {
    ProbeCase result{name, mandatory};

    QVideoWidget surface;
    surface.resize(320, 180);
    if (video)
        surface.show();

    QMediaPlayer player(nullptr, QMediaPlayer::VideoSurface);
    player.setVideoOutput(&surface);
    player.setMuted(true);
    player.setVolume(0);

    QString error;
    QElapsedTimer elapsed;
    elapsed.start();
    appendEvent(&result, elapsed, QStringLiteral("initialStatus"),
                mediaStatusName(player.mediaStatus()));
    appendEvent(&result, elapsed, QStringLiteral("initialState"), stateName(player.state()));
    appendEvent(&result, elapsed, QStringLiteral("availability"),
                QString::number(static_cast<int>(player.availability())));
    QObject::connect(&player, QOverload<QMediaPlayer::Error>::of(&QMediaPlayer::error),
                     &player, [&](QMediaPlayer::Error code) {
                         error = player.errorString();
                         appendEvent(&result, elapsed, QStringLiteral("error"),
                                     errorName(code) + QStringLiteral(": ") + error);
                     });
    QObject::connect(&player, &QMediaPlayer::mediaStatusChanged, &player,
                     [&](QMediaPlayer::MediaStatus status) {
                         appendEvent(&result, elapsed, QStringLiteral("mediaStatusChanged"),
                                     mediaStatusName(status));
                     });
    QObject::connect(&player, &QMediaPlayer::stateChanged, &player,
                     [&](QMediaPlayer::State state) {
                         appendEvent(&result, elapsed, QStringLiteral("stateChanged"),
                                     stateName(state));
                     });
    QObject::connect(&player, &QMediaPlayer::durationChanged, &player, [&](qint64 value) {
        appendEvent(&result, elapsed, QStringLiteral("durationChanged"),
                    QString::number(value));
    });
    QObject::connect(&player, &QMediaPlayer::videoAvailableChanged, &player, [&](bool value) {
        appendEvent(&result, elapsed, QStringLiteral("videoAvailableChanged"),
                    value ? QStringLiteral("true") : QStringLiteral("false"));
    });
    QObject::connect(&player, &QMediaPlayer::audioAvailableChanged, &player, [&](bool value) {
        appendEvent(&result, elapsed, QStringLiteral("audioAvailableChanged"),
                    value ? QStringLiteral("true") : QStringLiteral("false"));
    });
    QObject::connect(&player, &QMediaPlayer::seekableChanged, &player, [&](bool value) {
        appendEvent(&result, elapsed, QStringLiteral("seekableChanged"),
                    value ? QStringLiteral("true") : QStringLiteral("false"));
    });

    player.setMedia(content, device);
    appendEvent(&result, elapsed, QStringLiteral("setMedia"));
    player.play();
    appendEvent(&result, elapsed, QStringLiteral("play"));

    const bool loaded = waitUntil(7000, [&] {
        return player.mediaStatus() == QMediaPlayer::LoadedMedia ||
               player.mediaStatus() == QMediaPlayer::BufferedMedia ||
               player.mediaStatus() == QMediaPlayer::EndOfMedia ||
               player.state() == QMediaPlayer::PlayingState || !error.isEmpty();
    });
    result.loadMs = elapsed.elapsed();
    appendEvent(&result, elapsed, QStringLiteral("loadWaitFinished"),
                loaded ? QStringLiteral("true") : QStringLiteral("false"));
    if (!loaded || !error.isEmpty()) {
        result.error = error.isEmpty() ? QStringLiteral("media did not load") : error;
        return result;
    }

    const bool hasDuration = waitUntil(3000, [&] { return player.duration() > 0; });
    result.durationMs = player.duration();
    appendEvent(&result, elapsed, QStringLiteral("durationWaitFinished"),
                hasDuration ? QStringLiteral("true") : QStringLiteral("false"));
    if (!hasDuration) {
        result.error = QStringLiteral("duration was not reported");
        return result;
    }

    if (video) {
        const bool firstFrame = waitUntil(5000, [&] {
            QCoreApplication::processEvents();
            return imageHasVisiblePixels(surface.grab().toImage());
        });
        result.firstFrameMs = elapsed.elapsed();
        appendEvent(&result, elapsed, QStringLiteral("firstFrameWaitFinished"),
                    firstFrame ? QStringLiteral("true") : QStringLiteral("false"));
        if (!firstFrame) {
            result.error = QStringLiteral("first video frame was not visible");
            return result;
        }
    }

    const qint64 target = qBound<qint64>(250, player.duration() / 2, player.duration() - 250);
    player.setPosition(target);
    appendEvent(&result, elapsed, QStringLiteral("setPosition"), QString::number(target));
    const bool seeked = waitUntil(3000, [&] {
        return qAbs(player.position() - target) <= 250 || !error.isEmpty();
    });
    result.seekDeltaMs = qAbs(player.position() - target);
    appendEvent(&result, elapsed, QStringLiteral("seekWaitFinished"),
                seeked ? QStringLiteral("true") : QStringLiteral("false"));
    if (!seeked || !error.isEmpty()) {
        result.error = error.isEmpty() ? QStringLiteral("seek did not settle within 250 ms") : error;
        return result;
    }

    player.stop();
    result.passed = true;
    return result;
}

QString fixturePath(const ProbeOptions &options, const QString &name) {
    return QDir(options.fixtures).absoluteFilePath(name);
}

QMediaContent fileContent(const QString &path) {
    return QMediaContent(QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()));
}

bool writeJson(const ProbeOptions &options, const QJsonObject &root, QString *errorOut) {
    QFile output(options.jsonPath);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *errorOut = output.errorString();
        return false;
    }
    output.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    output.write("\n");
    return true;
}

} // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("wmf_media_probe"));

    QCommandLineParser parser;
    parser.addHelpOption();
    QCommandLineOption fixturesOption(QStringLiteral("fixtures"), QStringLiteral("Fixture directory."),
                                      QStringLiteral("path"));
    QCommandLineOption jsonOption(QStringLiteral("json"), QStringLiteral("JSON output path."),
                                  QStringLiteral("path"));
    QCommandLineOption uncOption(QStringLiteral("unc-root"),
                                 QStringLiteral("UNC fixture root, for example \\\\localhost\\FileCommanderMediaProbe."),
                                 QStringLiteral("path"));
    parser.addOption(fixturesOption);
    parser.addOption(jsonOption);
    parser.addOption(uncOption);
    parser.process(app);

    ProbeOptions options{parser.value(fixturesOption), parser.value(jsonOption),
                         parser.value(uncOption)};
    if (options.fixtures.isEmpty() || options.jsonPath.isEmpty()) {
        qCritical("Usage: wmf_media_probe --fixtures <dir> --json <path> [--unc-root <path>]");
        return 2;
    }

    QVector<ProbeCase> results;
    results.append(runPlaybackCase(QStringLiteral("wav-local"),
                                   fileContent(fixturePath(options, QStringLiteral("tone.wav"))),
                                   false, true));
    results.append(runPlaybackCase(QStringLiteral("mp3-local"),
                                   fileContent(fixturePath(options, QStringLiteral("tone.mp3"))),
                                   false, true));
    results.append(runPlaybackCase(QStringLiteral("mp4-h264-local"),
                                   fileContent(fixturePath(options, QStringLiteral("video-h264.mp4"))),
                                   true, true));

    QFile deviceFixture(fixturePath(options, QStringLiteral("video-h264.mp4")));
    if (deviceFixture.open(QIODevice::ReadOnly)) {
        results.append(runPlaybackCase(QStringLiteral("mp4-h264-qiodevice"), QMediaContent(),
                                       true, true, &deviceFixture));
    } else {
        results.append(ProbeCase{QStringLiteral("mp4-h264-qiodevice"), true, false,
                                 deviceFixture.errorString()});
    }

    if (options.uncRoot.isEmpty()) {
        results.append(ProbeCase{QStringLiteral("mp4-h264-unc"), true, false,
                                 QStringLiteral("UNC root was not supplied")});
    } else {
        const QString uncPath =
            QDir::fromNativeSeparators(options.uncRoot + QStringLiteral("/video-h264.mp4"));
        results.append(runPlaybackCase(QStringLiteral("mp4-h264-unc"), fileContent(uncPath), true,
                                       true));
    }

    results.append(runPlaybackCase(QStringLiteral("flac-local"),
                                   fileContent(fixturePath(options, QStringLiteral("tone.flac"))),
                                   false, false));
    results.append(runPlaybackCase(QStringLiteral("mkv-h264-local"),
                                   fileContent(fixturePath(options, QStringLiteral("video-h264.mkv"))),
                                   true, false));
    results.append(runPlaybackCase(QStringLiteral("hevc-local"),
                                   fileContent(fixturePath(options, QStringLiteral("video-hevc.mp4"))),
                                   true, false));

    bool accepted = true;
    QJsonArray cases;
    for (const ProbeCase &result : results) {
        cases.append(toJson(result));
        if (result.mandatory && !result.passed)
            accepted = false;
    }

    QJsonObject root;
    root.insert(QStringLiteral("probe"), QStringLiteral("wmf_media_probe"));
    root.insert(QStringLiteral("qtVersion"), QStringLiteral(QT_VERSION_STR));
    root.insert(QStringLiteral("preferredPlugin"),
                QString::fromLocal8Bit(qgetenv("QT_MULTIMEDIA_PREFERRED_PLUGINS")));
    root.insert(QStringLiteral("accepted"), accepted);
    root.insert(QStringLiteral("cases"), cases);

    QString writeError;
    if (!writeJson(options, root, &writeError)) {
        qCritical("Could not write probe JSON: %s", qPrintable(writeError));
        return 2;
    }
    return accepted ? 0 : 1;
}
