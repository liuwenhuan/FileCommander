#pragma once

#include <QString>
#include <QStringList>

// Shell-style command history cursor. add() records a submitted command;
// older()/newer() walk the cursor exactly as Up/Down do at a shell prompt.
// Pure logic (no widgets) so it can be unit tested directly.
class CommandHistory {
public:
    void add(const QString &command);

    // Up arrow: the previous (older) entry, or the oldest if already there.
    // `current` is returned unchanged when there is no history yet.
    QString older(const QString &current);

    // Down arrow: the next (newer) entry, or an empty string once back at the
    // fresh editing line.
    QString newer();

    void resetCursor() { m_cursor = m_items.size(); }
    const QStringList &items() const { return m_items; }

private:
    QStringList m_items;
    int m_cursor = 0; // index into m_items; == size() means the fresh new line
};
