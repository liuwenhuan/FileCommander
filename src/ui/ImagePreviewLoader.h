#pragma once

#include <QImage>
#include <QMetaType>
#include <QObject>
#include <QSize>
#include <QString>
#include <QTransform>

struct ImageMetadata {
    QString format;
    QSize dimensions;
    int depth = 0;
};

Q_DECLARE_METATYPE(ImageMetadata)

class ImagePreviewLoader : public QObject {
    Q_OBJECT

public:
    explicit ImagePreviewLoader(QObject *parent = nullptr);

    quint64 load(const QString &path);
    quint64 render(const QImage &source, QSize target, QTransform transform);
    void cancelBefore(quint64 generation);

signals:
    void loaded(quint64 generation, QImage image, ImageMetadata metadata, QString error);
    void rendered(quint64 generation, QImage image);

private:
    quint64 nextGeneration();

    quint64 m_nextGeneration = 0;
    quint64 m_cancelBeforeGeneration = 0;
};
