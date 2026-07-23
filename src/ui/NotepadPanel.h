#pragma once

#include <QSet>
#include <QString>
#include <QWidget>

#include "notepad/NotepadStore.h"

class QLineEdit;
class QPlainTextEdit;
class QTabWidget;
class QTimer;

// The quick-notepad third column: a search box, New/Delete buttons and a tab
// per note (each an auto-saving QPlainTextEdit). Self-contained -- it owns its
// NotepadStore and persists edits on a debounce timer, so the host only has to
// show/hide it and call saveAll() on close. Added by MainWindow as the third
// pane of the main splitter.
class NotepadPanel : public QWidget {
    Q_OBJECT

public:
    explicit NotepadPanel(QWidget *parent = nullptr);

    // Flushes every open note to disk (called by the host on window close and
    // whenever the panel is hidden). Also cancels the pending debounce save.
    void saveAll();

private slots:
    void onNewNote();
    void onDeleteCurrent();
    void onTabCloseRequested(int index);
    void onTabDoubleClicked(int index);
    void onEditorChanged();       // a note body changed -> mark dirty + arm timer
    void onSearchTextChanged(const QString &query);
    void flushPendingSaves();     // debounce timeout: write out the dirty notes

private:
    // Builds an editor tab for `note`, loading its body. Returns the tab index.
    int addNoteTab(const NotepadNote &note);
    // The note id bound to the tab at `index`, or empty if out of range.
    QString idAt(int index) const;

    NotepadStore m_store;
    QLineEdit *m_search = nullptr;
    QTabWidget *m_tabs = nullptr;
    QTimer *m_saveTimer = nullptr; // single-shot debounce for auto-save
    QSet<QString> m_dirty;         // note ids with unsaved edits
};
