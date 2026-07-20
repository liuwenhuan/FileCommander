#pragma once

#include <QPixmap>
#include <QWidget>

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

private:
    QWidget *buildImagePage();
    void applyImageScale();
    double fitScale() const;

    QStackedWidget *m_stack;
    QWidget *m_imagePage;
    QScrollArea *m_imageScroll;
    QLabel *m_imageLabel;
    QPlainTextEdit *m_text;
    QLabel *m_info;

    QPixmap m_originalPixmap;
    double m_imageScale = 1.0;
    bool m_imageFitMode = true; // re-fit on resize until the user zooms
};
