#pragma once

#include "FramelessWindow.h"

#include <QElapsedTimer>
#include <QQueue>
#include <QString>

class QLabel;
class QProgressBar;
class QTimer;

class IncomingTransferWindow : public FramelessWindow {
    Q_OBJECT

public:
    explicit IncomingTransferWindow(QWidget *parent = nullptr);

    void updateTransfer(const QString &id, const QString &fileName, qint64 written,
                        qint64 total, const QString &state);

protected:
    void changeEvent(QEvent *event) override;

private:
    struct Sample {
        qint64 milliseconds;
        qint64 bytes;
    };

    void refreshRate();
    void refreshDisplay();

    static constexpr qint64 kRateWindowMs = 2000;

    QLabel *m_fileLabel = nullptr;
    QLabel *m_stateLabel = nullptr;
    QLabel *m_bytesLabel = nullptr;
    QLabel *m_rateLabel = nullptr;
    QProgressBar *m_progressBar = nullptr;
    QTimer *m_rateTimer = nullptr;
    QElapsedTimer m_clock;
    QQueue<Sample> m_samples;
    QString m_id;
    QString m_fileName;
    QString m_state;
    qint64 m_written = 0;
    qint64 m_total = -1;
};
