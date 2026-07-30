#pragma once

#include <QString>

#include <memory>

class FileProvider;

enum class MediaKind { Audio, Video };

enum class MediaState { Idle, Loading, Playing, Paused, Ended, Failed };

struct MediaSource {
    QString path;
    std::shared_ptr<FileProvider> provider;
    bool isRemote = false;
};
