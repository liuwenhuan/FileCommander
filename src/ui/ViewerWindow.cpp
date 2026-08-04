#include "ViewerWindow.h"

#include <QFileInfo>
#include <QKeySequence>
#include <QShortcut>
#include <QVBoxLayout>

#include "QuickView.h"
#include "config/Settings.h"

ViewerWindow::ViewerWindow(Settings &settings, const QString &path, QWidget *parent)
    : FramelessWindow(parent) {
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QFileInfo(path).fileName());
    resize(900, 700);

    m_preview = new QuickView(settings, QuickView::Context::Window, this);
    m_preview->setContentFontSize(settings.listFontSize()); // match the app font size
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_preview);

    // Host-level shortcuts (kept out of the embedded pane so they don't steal
    // keys from the file list): Esc closes; F3 finds the next text match;
    // Left/Right step through sibling images.
    auto *esc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(esc, &QShortcut::activated, this, &QWidget::close);
    auto *findNext = new QShortcut(QKeySequence(Qt::Key_F3), this);
    connect(findNext, &QShortcut::activated, m_preview, &QuickView::findNext);
    auto *prev = new QShortcut(QKeySequence(Qt::Key_Left), this);
    connect(prev, &QShortcut::activated, m_preview, &QuickView::showPrevSibling);
    auto *next = new QShortcut(QKeySequence(Qt::Key_Right), this);
    connect(next, &QShortcut::activated, m_preview, &QuickView::showNextSibling);

    m_preview->showFile(path);
}

void ViewerWindow::refreshPhosphor() {
    if (m_preview)
        m_preview->refreshPhosphor();
}

void ViewerWindow::applyChromeFont(const QFont &font) {
    if (m_preview)
        m_preview->applyChromeFont(font);
}
