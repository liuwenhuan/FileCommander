#pragma once

#include <QWidget>

#include "AnimatedImage.h"

class QAction;

class QLabel;
class QScrollArea;

// Image viewer wired to F3 for image files. Supports fit/actual-size/
// zoom and next/previous navigation among sibling images in the same
// directory.
class ImageViewer : public QWidget {
    Q_OBJECT

public:
    explicit ImageViewer(QWidget *parent = nullptr);

    bool loadImage(const QString &path);

    static bool isImage(const QString &path);

public slots:
    void zoomIn();
    void zoomOut();
    void fitToWindow();
    void actualSize();
    void nextImage();
    void previousImage();

protected:
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void applyScale();
    void loadSiblingList();

    QScrollArea *m_scrollArea;
    QLabel *m_imageLabel;
    QPixmap m_pixmap;
    // Animation, when the file has more than one frame. Each frame is written
    // into m_pixmap and drawn through applyScale(), so zoom and fit keep
    // working on a GIF exactly as they do on a still -- there is one scaling
    // path, not two.
    AnimatedImage *m_animation = nullptr;
    QAction *m_playAction = nullptr;
    QString m_path;
    QStringList m_siblings;
    int m_siblingIndex = -1;
    double m_scale = 1.0;
    bool m_fitToWindow = true;
};
