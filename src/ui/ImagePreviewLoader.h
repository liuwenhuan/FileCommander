#pragma once

#include <QImage>
#include <QMetaType>
#include <QObject>
#include <QSize>
#include <QString>
#include <QTransform>

#include <functional>
#include <memory>

struct ImageMetadata {
    QString format;
    QSize dimensions;
    int depth = 0;
};

Q_DECLARE_METATYPE(ImageMetadata)

struct ImagePreviewLoaderState;
struct ImagePreviewLoaderDeliveryToken;

class ImagePreviewLoader : public QObject {
    Q_OBJECT

public:
    enum class WorkerCheckpoint {
        LoadBeforeDecode,
        LoadWaitingForFile,
        LoadAfterDecode,
        RenderBeforeTransform,
        RenderAfterTransform,
        RenderAfterScale,
        RotationBeforeWrite,
        RotationAfterWrite,
    };

    explicit ImagePreviewLoader(QObject *parent = nullptr);
    ~ImagePreviewLoader() override;

    quint64 load(const QString &path);
    quint64 render(const QImage &source, QSize target, QTransform transform);
    void persistRotation(const QString &path, int degrees);
    void cancelBefore(quint64 generation);

    void setWorkerCheckpointForTest(
        std::function<void(WorkerCheckpoint, quint64)> checkpoint);
    bool waitForIdleForTest(int timeoutMs);
    static bool waitForAllForTest(int timeoutMs);

signals:
    void loaded(quint64 generation, QImage image, ImageMetadata metadata, QString error);
    void rendered(quint64 generation, QImage image);
    void rotationPersisted(QString path, bool saved);

private:
    quint64 nextGeneration();

    quint64 m_nextGeneration = 0;
    std::shared_ptr<ImagePreviewLoaderState> m_state;
    std::shared_ptr<ImagePreviewLoaderDeliveryToken> m_deliveryToken;
};
