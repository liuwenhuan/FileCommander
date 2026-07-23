#include "NotepadPanel.h"

#include <QColor>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabBar>
#include <QTabWidget>
#include <QTextCursor>
#include <QTimer>
#include <QVBoxLayout>

namespace {
constexpr int kAutoSaveDelayMs = 600; // debounce between the last keystroke and a save

// Each editor carries its note id as a dynamic property so a textChanged signal
// can be traced back to the note it belongs to without a parallel container.
const char kNoteIdProperty[] = "notepadNoteId";
} // namespace

NotepadPanel::NotepadPanel(QWidget *parent) : QWidget(parent) {
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Search all notes..."));
    m_search->setClearButtonEnabled(true);

    auto *newButton = new QPushButton(tr("New"), this);
    auto *deleteButton = new QPushButton(tr("Delete"), this);

    auto *toolRow = new QHBoxLayout;
    toolRow->setContentsMargins(0, 0, 0, 0);
    toolRow->addWidget(m_search, 1);
    toolRow->addWidget(newButton);
    toolRow->addWidget(deleteButton);

    m_tabs = new QTabWidget(this);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->setDocumentMode(true);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);
    layout->addLayout(toolRow);
    layout->addWidget(m_tabs, 1);

    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(kAutoSaveDelayMs);
    connect(m_saveTimer, &QTimer::timeout, this, &NotepadPanel::flushPendingSaves);

    connect(newButton, &QPushButton::clicked, this, &NotepadPanel::onNewNote);
    connect(deleteButton, &QPushButton::clicked, this, &NotepadPanel::onDeleteCurrent);
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &NotepadPanel::onTabCloseRequested);
    connect(m_tabs, &QTabWidget::tabBarDoubleClicked, this, &NotepadPanel::onTabDoubleClicked);
    connect(m_search, &QLineEdit::textChanged, this, &NotepadPanel::onSearchTextChanged);

    // Load every saved note as a tab; seed an empty note on first run so the
    // panel is never blank.
    const QVector<NotepadNote> notes = m_store.notes();
    if (notes.isEmpty()) {
        addNoteTab(m_store.create(tr("Note 1")));
    } else {
        for (const NotepadNote &note : notes)
            addNoteTab(note);
    }
}

int NotepadPanel::addNoteTab(const NotepadNote &note) {
    auto *editor = new QPlainTextEdit(m_tabs);
    editor->setPlainText(m_store.load(note.id));
    editor->setProperty(kNoteIdProperty, note.id);
    connect(editor, &QPlainTextEdit::textChanged, this, &NotepadPanel::onEditorChanged);
    const int index = m_tabs->addTab(editor, note.title);
    return index;
}

QString NotepadPanel::idAt(int index) const {
    QWidget *w = m_tabs->widget(index);
    return w ? w->property(kNoteIdProperty).toString() : QString();
}

void NotepadPanel::onNewNote() {
    const NotepadNote note = m_store.create(tr("Note %1").arg(m_tabs->count() + 1));
    const int index = addNoteTab(note);
    m_tabs->setCurrentIndex(index);
}

void NotepadPanel::onDeleteCurrent() {
    onTabCloseRequested(m_tabs->currentIndex());
}

void NotepadPanel::onTabCloseRequested(int index) {
    const QString id = idAt(index);
    if (id.isEmpty())
        return;

    const QString title = m_tabs->tabText(index);
    const auto reply = QMessageBox::question(
        this, tr("Delete Note"),
        tr("Delete note \"%1\"? This cannot be undone.").arg(title),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes)
        return;

    m_dirty.remove(id);
    m_store.remove(id);
    QWidget *editor = m_tabs->widget(index);
    m_tabs->removeTab(index);
    delete editor;

    // Never leave the panel empty: reseed a fresh note.
    if (m_tabs->count() == 0)
        addNoteTab(m_store.create(tr("Note 1")));
}

void NotepadPanel::onTabDoubleClicked(int index) {
    const QString id = idAt(index);
    if (id.isEmpty())
        return;

    bool ok = false;
    const QString title = QInputDialog::getText(this, tr("Rename Note"), tr("Title:"),
                                                QLineEdit::Normal, m_tabs->tabText(index), &ok);
    if (!ok || title.isEmpty())
        return;

    m_store.rename(id, title);
    m_tabs->setTabText(index, title);
}

void NotepadPanel::onEditorChanged() {
    auto *editor = qobject_cast<QPlainTextEdit *>(sender());
    if (!editor)
        return;
    const QString id = editor->property(kNoteIdProperty).toString();
    if (!id.isEmpty())
        m_dirty.insert(id);
    m_saveTimer->start(); // restart the debounce window
}

void NotepadPanel::flushPendingSaves() {
    if (m_dirty.isEmpty())
        return;
    for (int i = 0; i < m_tabs->count(); ++i) {
        const QString id = idAt(i);
        if (id.isEmpty() || !m_dirty.contains(id))
            continue;
        if (auto *editor = qobject_cast<QPlainTextEdit *>(m_tabs->widget(i)))
            m_store.save(id, editor->toPlainText());
    }
    m_dirty.clear();
}

void NotepadPanel::onSearchTextChanged(const QString &query) {
    const QColor hitColor = palette().color(QPalette::Highlight);

    if (query.isEmpty()) {
        // Clear tab highlighting and any in-editor selection.
        for (int i = 0; i < m_tabs->count(); ++i)
            m_tabs->tabBar()->setTabTextColor(i, QColor());
        if (auto *editor = qobject_cast<QPlainTextEdit *>(m_tabs->currentWidget())) {
            QTextCursor cursor = editor->textCursor();
            cursor.clearSelection();
            editor->setTextCursor(cursor);
        }
        return;
    }

    const QVector<QString> hits = m_store.search(query);

    // Tint tabs that contain a match with the palette highlight colour, reset
    // the rest to the theme default.
    for (int i = 0; i < m_tabs->count(); ++i)
        m_tabs->tabBar()->setTabTextColor(i, hits.contains(idAt(i)) ? hitColor : QColor());

    // Highlight the first match inside the current tab, searching from its top.
    if (auto *editor = qobject_cast<QPlainTextEdit *>(m_tabs->currentWidget())) {
        editor->moveCursor(QTextCursor::Start);
        editor->find(query);
    }
}

void NotepadPanel::saveAll() {
    m_saveTimer->stop();
    for (int i = 0; i < m_tabs->count(); ++i) {
        const QString id = idAt(i);
        if (id.isEmpty())
            continue;
        if (auto *editor = qobject_cast<QPlainTextEdit *>(m_tabs->widget(i)))
            m_store.save(id, editor->toPlainText());
    }
    m_dirty.clear();
}
