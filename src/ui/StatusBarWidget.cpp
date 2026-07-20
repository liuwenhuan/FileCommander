#include "StatusBarWidget.h"

#include <QHBoxLayout>
#include <QLabel>

StatusBarWidget::StatusBarWidget(QWidget *parent) : QWidget(parent) {
    m_label = new QLabel(this);
    m_diskLabel = new QLabel(this);
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 0, 6, 0);
    layout->addWidget(m_label);
    layout->addStretch(1);
    layout->addWidget(m_diskLabel);
}

namespace {
QString humanSize(qint64 bytes) {
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double size = static_cast<double>(bytes);
    int unit = 0;
    while (size >= 1024.0 && unit < 4) {
        size /= 1024.0;
        ++unit;
    }
    if (unit == 0)
        return QStringLiteral("%1 B").arg(bytes);
    return QStringLiteral("%1 %2").arg(size, 0, 'f', 1).arg(units[unit]);
}
} // namespace

void StatusBarWidget::setSelectionInfo(int selectedCount, qint64 selectedBytes, int totalCount) {
    if (selectedCount == 0) {
        m_label->setText(QObject::tr("%1 object(s)").arg(totalCount));
    } else {
        m_label->setText(QObject::tr("%1 of %2 selected, %3")
                              .arg(selectedCount)
                              .arg(totalCount)
                              .arg(humanSize(selectedBytes)));
    }
}

void StatusBarWidget::setDiskInfo(qint64 freeBytes, qint64 totalBytes) {
    if (totalBytes <= 0) {
        m_diskLabel->clear();
        return;
    }
    m_diskLabel->setText(
        QObject::tr("%1 free of %2").arg(humanSize(freeBytes), humanSize(totalBytes)));
}
