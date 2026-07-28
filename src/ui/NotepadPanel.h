#pragma once

#include <QRect>
#include <QString>
#include <QWidget>

#include <memory>

#include "config/Settings.h"
#include "notepad/NotepadStore.h"

class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QPushButton;
class QSplitter;
class QTimer;

// The quick-notepad fly-out: a floating popup (like the external-connection
// panel) anchored above its launching button rather than a docked column. It is
// split top/bottom by a draggable divider:
//   * the top pane is a scrollable list of notes, each row previewing the
//     note's title / first line;
//   * the bottom pane edits the note selected above.
// It owns its NotepadStore, auto-saves edits on a debounce, flushes on close,
// and deletes itself when closed (WA_DeleteOnClose).
class NotepadPanel : public QWidget {
    Q_OBJECT

public:
    explicit NotepadPanel(QWidget *parent = nullptr);
    NotepadPanel(Settings &settings, QWidget *parent = nullptr);
    NotepadPanel(Settings &settings, const QString &notepadDirectory, QWidget *parent = nullptr);

    // Shows the panel as a fly-out above the launching button: its RIGHT edge is
    // aligned to the app window's right edge, its bottom sits just above
    // anchorGlobalRect (the button), and its height is capped so the top edge
    // never rises above appContentGlobalRect.top(). appContentGlobalRect is the
    // app window's VISIBLE content rect in global coords (shadow margin excluded).
    void popUpAbove(const QRect &anchorGlobalRect, const QRect &appContentGlobalRect);

    // Flushes the in-editor note to disk. Called on close.
    void saveAll();

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onNewNote();
    void onDeleteCurrent();
    void onCurrentRowChanged();               // list selection -> load into editor
    void onEditorChanged();                   // editor edited -> dirty + arm timer
    void onSearchTextChanged(const QString &query);
    void flushPendingSaves();                 // debounce timeout: persist the note

private:
    void initialize();
    // (Re)computes the dynamic height + splitter split from the current note
    // count and repositions the popup above its anchor. No-op until popUpAbove
    // has recorded an anchor.
    void applyDynamicSize();
    // Rebuilds the list from the store, selecting `selectId` (or the first row).
    void reloadList(const QString &selectId = QString());
    QListWidgetItem *addRow(const NotepadNote &note);
    QString currentId() const;
    // Writes the editor's text to its note and refreshes that row's preview.
    void commitCurrentEditor();
    // Reverts the Delete button from its armed "confirm" state.
    void disarmDelete();
    // Preview shown in the list: the first non-empty line of the body, trimmed.
    static QString previewOf(const QString &body);

    std::unique_ptr<Settings> m_ownedSettings;
    Settings &m_settings;
    NotepadStore m_store;
    QLineEdit *m_search = nullptr;
    QListWidget *m_list = nullptr;
    QSplitter *m_splitter = nullptr;
    QPlainTextEdit *m_editor = nullptr;
    QPushButton *m_deleteButton = nullptr;
    QTimer *m_saveTimer = nullptr;
    QTimer *m_deleteArmTimer = nullptr; // reverts the two-step delete confirm

    QString m_currentId;          // note bound to the editor
    bool m_dirty = false;         // editor has unsaved edits
    bool m_loadingEditor = false; // guard: suppress textChanged while loading
    bool m_deleteArmed = false;   // Delete clicked once, waiting for confirm

    QRect m_anchorRect;           // launching button geometry (for re-fitting)
    QRect m_appContentRect;       // app window's visible content rect (no shadow):
                                  // right edge to align to, top edge as the cap

    int m_editorHeight = 0;       // persisted editor-pane height (px); 0 = unset,
                                  // fall back to the preferred height
};
