#pragma once

#include <QPixmap>
#include <QPoint>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QSlider;
class QStackedWidget;
class QTimer;
class MpvWidget;

// Lightweight in-panel preview shown by Ctrl+Q: renders the file under the
// cursor as a zoomable image, a text head, or a "no preview" note.
class QuickView : public QWidget {
    Q_OBJECT

public:
    explicit QuickView(QWidget *parent = nullptr);

    void showFile(const QString &path);

protected:
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

    static bool isVideo(const QString &path);

private:
    QWidget *buildImagePage();
    QWidget *buildVideoPage();
    void applyImageScale();
    void zoomImageBy(double factor);
    double fitScale() const;
    void positionInfoOverlay();      // keep the image metadata panel pinned top-right
    void positionVideoInfoOverlay(); // same, over the video area
    void updateVideoInfoOverlay();   // refresh the video metadata text
    void stopVideo();                // unload + hide the video page

    QStackedWidget *m_stack;
    QWidget *m_imagePage;
    QScrollArea *m_imageScroll;
    QLabel *m_imageLabel;
    QCheckBox *m_lockZoomCheck; // when checked, keep the ratio for later images
    QCheckBox *m_infoCheck;     // when checked, overlay image metadata
    QLabel *m_infoOverlay;      // floating metadata panel over the image
    QTimer *m_refitTimer;       // coalesces refits during interactive resize
    QPlainTextEdit *m_text;
    QLabel *m_info;

    // Video page (m_stack index 3).
    QWidget *m_videoPage = nullptr;
    MpvWidget *m_mpv = nullptr;
    QPushButton *m_playButton = nullptr;
    QComboBox *m_speedCombo = nullptr;
    QSlider *m_progressSlider = nullptr;
    QSlider *m_volumeSlider = nullptr;
    QCheckBox *m_videoInfoCheck = nullptr;
    QLabel *m_videoInfoOverlay = nullptr;
    QTimer *m_videoTimer = nullptr; // polls position while playing
    bool m_seeking = false;         // suppress timer updates while dragging

    QPixmap m_originalPixmap;
    double m_imageScale = 1.0;
    bool m_imageFitMode = true; // re-fit on resize until the user zooms

    // Left-drag panning state.
    bool m_panning = false;
    QPoint m_panStart;
    int m_panHScroll = 0;
    int m_panVScroll = 0;
};
