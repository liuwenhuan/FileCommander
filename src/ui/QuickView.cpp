#include "QuickView.h"

#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "ImageViewer.h"

namespace {
constexpr qint64 kTextPreviewBytes = 64 * 1024; // cap text previews at 64 KiB
}

QuickView::QuickView(QWidget *parent) : QWidget(parent) {
    m_image = new QLabel(this);
    m_image->setAlignment(Qt::AlignCenter);
    m_image->setMinimumSize(1, 1);

    m_text = new QPlainTextEdit(this);
    m_text->setReadOnly(true);
    m_text->setLineWrapMode(QPlainTextEdit::NoWrap);

    m_info = new QLabel(tr("Select a file to preview"), this);
    m_info->setAlignment(Qt::AlignCenter);
    m_info->setWordWrap(true);

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(m_info);  // 0
    m_stack->addWidget(m_image); // 1
    m_stack->addWidget(m_text);  // 2

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_stack);
}

void QuickView::showFile(const QString &path) {
    QFileInfo info(path);
    if (path.isEmpty() || !info.exists() || info.isDir()) {
        m_info->setText(tr("Select a file to preview"));
        m_stack->setCurrentWidget(m_info);
        return;
    }

    if (ImageViewer::isImage(path)) {
        QPixmap pm(path);
        if (!pm.isNull()) {
            m_image->setPixmap(pm.scaled(m_image->size(), Qt::KeepAspectRatio,
                                          Qt::SmoothTransformation));
            m_stack->setCurrentWidget(m_image);
            return;
        }
    }

    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        const QByteArray head = file.read(kTextPreviewBytes);
        m_text->setPlainText(QString::fromUtf8(head));
        m_stack->setCurrentWidget(m_text);
        return;
    }

    m_info->setText(tr("No preview available for %1").arg(info.fileName()));
    m_stack->setCurrentWidget(m_info);
}
