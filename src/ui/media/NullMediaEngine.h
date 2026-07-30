#pragma once

#include "MediaEngine.h"

class NullMediaEngine final : public MediaEngine {
public:
    explicit NullMediaEngine(QObject *parent = nullptr);

    void initialize() override;
    void load(const MediaSource &source, MediaKind kind) override;
    QWidget *videoSurface() override;
};
