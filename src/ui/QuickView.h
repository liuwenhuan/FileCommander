#pragma once

#include <QPixmap>
#include <QPoint>
#include <QWidget>

class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QScrollArea;
class QStackedWidget;

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

private:
    QWidget *buildImagePage();
    void applyImageScale();
    void zoomImageBy(double factor);
    double fitScale() const;

    QStackedWidget *m_stack;
    QWidget *m_imagePage;
    QScrollArea *m_imageScroll;
    QLabel *m_imageLabel;
    QCheckBox *m_lockZoomCheck; // when checked, keep the ratio for later images
    QPlainTextEdit *m_text;
    QLabel *m_info;

    QPixmap m_originalPixmap;
    double m_imageScale = 1.0;
    bool m_imageFitMode = true; // re-fit on resize until the user zooms

    // Left-drag panning state.
    bool m_panning = false;
    QPoint m_panStart;
    int m_panHScroll = 0;
    int m_panVScroll = 0;
};
