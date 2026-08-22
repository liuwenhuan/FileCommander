#pragma once

#include <QKeySequence>
#include <QList>
#include <QMap>
#include <QPair>
#include <QString>
#include <QStringList>

#include <functional>

class QShortcut;
class QWidget;

// Every command the application can run, by id: its label, its key, and what it
// does.
//
// This was five parallel QMaps and a bool inside MainWindow, read from
// seventeen different places -- the menus, the function-key bar, the "*"
// command list, the shortcuts dialog, the language-change path. Keeping them in
// step was the caller's job every time, and the invariants between them (a
// keyed command appears in the order list, a re-registration refreshes the
// label but must NOT create a second QShortcut) lived only in comments.
//
// Nothing here knows about Settings or about MainWindow. The caller resolves a
// command's current key -- from settings, or from the default -- and hands it
// in; the registry owns the QShortcut objects from then on.
class CommandRegistry {
public:
    // Shortcuts are parented to `shortcutParent` and live as long as it does.
    explicit CommandRegistry(QWidget *shortcutParent);

    // A command with a key of its own. Safe to call again for the same id: the
    // label is refreshed (which is what a language change needs) and the
    // QShortcut is left alone. Creating it twice would make the key fire twice.
    void bind(const QString &id, const QString &label, const QKeySequence &defaultSequence,
              const QKeySequence &currentSequence, std::function<void()> handler);

    // A command reachable from menus and the command list but with no key.
    // Unlike bind(), an existing handler is kept rather than replaced.
    void registerCommand(const QString &id, const QString &label, std::function<void()> handler);

    // Records what key a command would have by default even though the key
    // itself belongs to a reassignable slot. The change dialog shows it.
    void setDefaultSequence(const QString &id, const QKeySequence &sequence);

    // Runs the command if it exists and has a handler. Returns whether it ran,
    // so a caller can tell "no such command" from "did nothing".
    bool run(const QString &id) const;
    std::function<void()> handler(const QString &id) const;

    // The id itself when the command is unknown, so a stale binding shows
    // something rather than an empty menu entry.
    QString label(const QString &id) const;
    bool contains(const QString &id) const;

    // The live key if one is bound, otherwise the recorded default.
    QKeySequence sequence(const QString &id) const;

    // Every labelled command, for the picker.
    QStringList ids() const;
    // id and label of the keyed commands only, in registration order, which is
    // the order the shortcuts dialog lists them in.
    QList<QPair<QString, QString>> keyedOrder() const;
    QMap<QString, QKeySequence> defaults() const;
    QMap<QString, QKeySequence> currentSequences() const;

    // Ignores ids that have no shortcut, so a dialog result cannot invent one.
    void setSequence(const QString &id, const QKeySequence &sequence);

    // Gives standard editing commands (copy/cut/paste) to the focused text
    // widget before a window-wide file command consumes the same shortcut.
    // Returns true when a focus widget or one of its parents exposes `method()`.
    static bool invokeFocusedWidgetCommand(const char *method);

    // One-shot guard for the connections that must not be made twice when
    // setupShortcuts() is re-run after a language change.
    bool isBuilt() const { return m_built; }
    void markBuilt() { m_built = true; }

private:
    QWidget *m_shortcutParent = nullptr;
    bool m_built = false;

    QMap<QString, QShortcut *> m_shortcuts;
    QMap<QString, QKeySequence> m_defaults;
    QMap<QString, std::function<void()>> m_handlers;
    QMap<QString, QString> m_labels;
    QList<QPair<QString, QString>> m_keyedOrder;
};
