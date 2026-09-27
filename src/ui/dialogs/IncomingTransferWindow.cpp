#include "IncomingTransferWindow.h"

#include <QEvent>
#include <QLabel>
#include <QProgressBar>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>

namespace {

QString humanBytes(qint64 bytes) {
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double size = static_cast<double>(qMax<qint64>(0, bytes));
    int unit = 0;
    while (size >= 1024.0 && unit < 4) {
        size /= 1024.0;
        ++unit;
    }
    return unit == 0 ? QStringLiteral("%1 B").arg(bytes)
                     : QStringLiteral("%1 %2").arg(size, 0, 'f', 1).arg(units[unit]);
}

} // namespace

IncomingTransferWindow::IncomingTransferWindow(QWidget *parent) : FramelessWindow(parent) {
    setWindowTitle(tr("Receiving file"));
    setAttribute(Qt::WA_ShowWithoutActivating);
    resize(440, 190);

    auto *layout = new QVBoxLayout(this);
    m_fileLabel = new QLabel(this);
    m_fileLabel->setObjectName(QStringLiteral("IncomingTransferFile"));
    m_fileLabel->setWordWrap(true);
    m_fileLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_stateLabel = new QLabel(this);
    m_stateLabel->setObjectName(QStringLiteral("IncomingTransferState"));
    m_progressBar = new QProgressBar(this);
    m_progressBar->setObjectName(QStringLiteral("IncomingTransferProgress"));
    m_progressBar->setRange(0, 0);
    m_bytesLabel = new QLabel(this);
    m_bytesLabel->setObjectName(QStringLiteral("IncomingTransferBytes"));
    m_rateLabel = new QLabel(this);
    m_rateLabel->setObjectName(QStringLiteral("IncomingTransferRate"));
    layout->addWidget(m_fileLabel);
    layout->addWidget(m_stateLabel);
    layout->addWidget(m_progressBar);
    layout->addWidget(m_bytesLabel);
    layout->addWidget(m_rateLabel);

    m_rateTimer = new QTimer(this);
    m_rateTimer->setInterval(250);
    connect(m_rateTimer, &QTimer::timeout, this, &IncomingTransferWindow::refreshRate);
}

void IncomingTransferWindow::changeEvent(QEvent *event) {
    FramelessWindow::changeEvent(event);
    if (event->type() == QEvent::LanguageChange && m_fileLabel) {
        setWindowTitle(tr("Receiving file"));
        refreshDisplay();
    }
}

void IncomingTransferWindow::updateTransfer(const QString &id, const QString &fileName,
                                            qint64 written, qint64 total,
                                            const QString &state) {
    if (id.isEmpty() || (state != QLatin1String("receiving") &&
                         state != QLatin1String("complete") &&
                         state != QLatin1String("failed") &&
                         state != QLatin1String("interrupted")))
        return;

    const bool newTransfer = id != m_id;
    if (newTransfer && state != QLatin1String("receiving"))
        return;
    if (newTransfer) {
        m_id = id;
        m_samples.clear();
        m_clock.start();
        m_written = 0;
        m_total = -1;
    } else if (m_state != QLatin1String("receiving")) {
        return;
    }

    m_fileName = fileName;
    m_written = qMax(m_written, qMax<qint64>(0, written));
    m_total = total;
    m_state = state;
    if (newTransfer)
        m_samples.enqueue({0, m_written});
    else if (state == QLatin1String("receiving"))
        m_samples.enqueue({m_clock.elapsed(), m_written});

    refreshDisplay();
    if (state == QLatin1String("receiving")) {
        if (!m_rateTimer->isActive())
            m_rateTimer->start();
    } else {
        m_rateTimer->stop();
        m_samples.clear();
    }

    if (newTransfer)
        show();
}

void IncomingTransferWindow::refreshRate() {
    if (m_state != QLatin1String("receiving"))
        return;
    const qint64 now = m_clock.elapsed();
    m_samples.enqueue({now, m_written});
    while (m_samples.size() > 1 && m_samples.head().milliseconds < now - kRateWindowMs)
        m_samples.dequeue();
    refreshDisplay();
}

void IncomingTransferWindow::refreshDisplay() {
    m_fileLabel->setText(m_fileName);
    if (m_state == QLatin1String("complete"))
        m_stateLabel->setText(tr("Complete"));
    else if (m_state == QLatin1String("failed"))
        m_stateLabel->setText(tr("Failed"));
    else if (m_state == QLatin1String("interrupted"))
        m_stateLabel->setText(tr("Interrupted"));
    else
        m_stateLabel->setText(tr("Receiving"));

    if (m_total > 0) {
        m_progressBar->setRange(0, 1000);
        const int scaled = static_cast<int>(qMin(1000.0,
            static_cast<double>(m_written) / static_cast<double>(m_total) * 1000.0));
        m_progressBar->setValue(m_state == QLatin1String("complete")
                                    ? scaled : qMin(999, scaled));
        m_bytesLabel->setText(tr("Received: %1 / %2").arg(humanBytes(m_written),
                                                         humanBytes(m_total)));
    } else {
        m_progressBar->setRange(0, 0);
        m_bytesLabel->setText(tr("Received: %1").arg(humanBytes(m_written)));
    }

    qint64 bytesPerSecond = 0;
    if (m_state == QLatin1String("receiving") && m_samples.size() > 1) {
        const Sample &first = m_samples.head();
        const Sample &last = m_samples.last();
        const qint64 elapsed = last.milliseconds - first.milliseconds;
        if (elapsed > 0)
            bytesPerSecond = (last.bytes - first.bytes) * 1000 / elapsed;
    }
    m_rateLabel->setText(tr("Receiving: %1/s").arg(humanBytes(bytesPerSecond)));
}
