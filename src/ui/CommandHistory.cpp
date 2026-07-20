#include "CommandHistory.h"

void CommandHistory::add(const QString &command) {
    if (command.isEmpty()) {
        resetCursor();
        return;
    }
    // Collapse immediate repeats, like bash's ignoredups.
    if (m_items.isEmpty() || m_items.last() != command)
        m_items.append(command);
    resetCursor();
}

QString CommandHistory::older(const QString &current) {
    if (m_items.isEmpty())
        return current;
    if (m_cursor > 0)
        --m_cursor;
    return m_items.at(m_cursor);
}

QString CommandHistory::newer() {
    if (m_cursor < m_items.size())
        ++m_cursor;
    return m_cursor < m_items.size() ? m_items.at(m_cursor) : QString();
}
