#pragma once

#include <QColor>
#include <QString>

#include <memory>

class FileProvider;

enum class MediaKind { Audio, Video };

enum class MediaState { Idle, Loading, Playing, Paused, Ended, Failed };

struct MediaSource {
    QString path;
    std::shared_ptr<FileProvider> provider;
    bool isRemote = false;
    // Optional seekable local copy for a remote source. Windows Media Foundation
    // opens this instead of the provider path; mpv keeps using provider-backed
    // stream URLs and does not need it.
    QString localCachePath;
};

struct VideoEffectSettings {
    QColor tint;
    int pixelBlock = 0;

    bool enabled() const { return tint.isValid(); }
};
