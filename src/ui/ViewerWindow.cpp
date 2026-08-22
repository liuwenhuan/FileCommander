#include "ViewerWindow.h"

#include <QCloseEvent>
#include <QFileInfo>
#include <QKeySequence>
#include <QShortcut>
#include <QVBoxLayout>

#include "QuickView.h"
#include "config/Settings.h"

ViewerWindow::ViewerWindow(Settings &settings, const QString &path, QWidget *parent,
                           bool editing)
    : ViewerWindow(settings, path, parent, editing, QString()) {}

ViewerWindow::ViewerWindow(Settings &settings, const QString &path, QWidget *parent,
                           bool editing, const QString &encodingIdentity)
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

    // The preview switches into the editor itself, in place; nothing above this
    // window needs to know. While it is editing, the shortcuts above would eat
    // the editor's own keys (Esc, F3 find, cursor keys), so they stand down.
    connect(m_preview, &QuickView::editRequested, m_preview,
            [this](const QString &file) { m_preview->beginEditing(file); });
    connect(m_preview, &QuickView::editingChanged, this, [this](bool editing) {
        for (QShortcut *shortcut : findChildren<QShortcut *>())
            shortcut->setEnabled(!editing);
    });

    // In edit mode, INSTEAD of showFile(): its async text probe would finish
    // later and reveal the preview page over the editor.
    if (!editing || !m_preview->beginEditing(path, encodingIdentity))
        m_preview->showFile(path, encodingIdentity);
}

bool ViewerWindow::beginEditing(const QString &path) {
    return beginEditing(path, QString());
}

bool ViewerWindow::beginEditing(const QString &path, const QString &encodingIdentity) {
    return m_preview && m_preview->beginEditing(path, encodingIdentity);
}

void ViewerWindow::closeEvent(QCloseEvent *event) {
    // The embedded editor is a child widget, so its own closeEvent never fires.
    if (m_preview && !m_preview->confirmDiscardEdits())
        event->ignore();
    else
        FramelessWindow::closeEvent(event);
}

void ViewerWindow::setEditingEnabled(bool enabled) {
    if (m_preview)
        m_preview->setTextEditingEnabled(enabled);
}

void ViewerWindow::refreshPhosphor() {
    if (m_preview)
        m_preview->refreshPhosphor();
}

void ViewerWindow::applyChromeFont(const QFont &font) {
    if (m_preview)
        m_preview->applyChromeFont(font);
}
