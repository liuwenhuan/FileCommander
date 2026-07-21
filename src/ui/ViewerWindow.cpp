#include "ViewerWindow.h"

#include <QFileInfo>
#include <QKeySequence>
#include <QShortcut>
#include <QVBoxLayout>

#include "QuickView.h"

ViewerWindow::ViewerWindow(Settings &settings, const QString &path, QWidget *parent)
    : QWidget(parent) {
    setWindowFlag(Qt::Window);
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QFileInfo(path).fileName());
    resize(900, 700);

    m_preview = new QuickView(settings, QuickView::Context::Window, this);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_preview);

    // Esc closes the viewer (matches the old TextViewer/ImageViewer behaviour).
    auto *esc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(esc, &QShortcut::activated, this, &QWidget::close);

    m_preview->showFile(path);
}
