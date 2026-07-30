#include "ThumbnailSweep.h"

void ThumbnailSweep::reset(int rowCount) {
    const int rows = rowCount > 0 ? rowCount : 0;
    m_taken = QBitArray(rows);
    m_cursor = 0;
    m_takenAt = 0;
    m_remaining = rows;
    m_foregroundRows.clear();
    m_nextForegroundRow = 0;
}

void ThumbnailSweep::focusOn(int firstVisible, int lastVisible) {
    Q_UNUSED(lastVisible);
    const int rows = m_taken.size();
    if (rows == 0)
        return;
    m_foregroundRows.clear();
    m_nextForegroundRow = 0;
    m_cursor = qBound(0, firstVisible, rows - 1);
}

void ThumbnailSweep::focusVisibleRowsWithAdjacentViewports(int firstVisible, int lastVisible) {
    const int rows = m_taken.size();
    if (rows == 0)
        return;

    const int first = qBound(0, firstVisible, rows - 1);
    const int last = qBound(first, lastVisible, rows - 1);
    const int viewportRows = last - first + 1;
    const int adjacentRows = 2 * viewportRows;

    m_foregroundRows.clear();
    m_foregroundRows.reserve(viewportRows + 2 * adjacentRows);
    for (int row = first; row <= last; ++row)
        m_foregroundRows.append(row);
    for (int row = last + 1; row <= qMin(rows - 1, last + adjacentRows); ++row)
        m_foregroundRows.append(row);
    for (int row = first - 1; row >= qMax(0, first - adjacentRows); --row)
        m_foregroundRows.append(row);
    m_nextForegroundRow = 0;
    m_cursor = first;
}

void ThumbnailSweep::putBack(int row) {
    if (row < 0 || row >= m_taken.size() || !m_taken.testBit(row))
        return;
    m_taken.clearBit(row);
    ++m_remaining;
    // Only rewind to `row` when nothing more urgent has been aimed at since it
    // was taken. A focusOn() between next() and putBack() means the user has
    // scrolled somewhere else, and that new position outranks a row we merely
    // failed to place -- rewinding unconditionally would silently serve the
    // abandoned region first. Cleared above either way, so the row is never
    // lost: the scan picks it up when it comes back around.
    const bool noRefocusSinceTake = m_cursor == m_takenAt;
    if (noRefocusSinceTake)
        m_cursor = row;
    if (noRefocusSinceTake && m_nextForegroundRow > 0
        && m_foregroundRows[m_nextForegroundRow - 1] == row)
        --m_nextForegroundRow;
}

int ThumbnailSweep::next(bool *foreground) {
    if (foreground)
        *foreground = false;
    const int rows = m_taken.size();
    if (rows == 0 || m_remaining <= 0)
        return -1;

    while (m_nextForegroundRow < m_foregroundRows.size()) {
        const int row = m_foregroundRows.at(m_nextForegroundRow++);
        if (m_taken.testBit(row))
            continue;
        m_taken.setBit(row);
        --m_remaining;
        m_cursor = (row + 1) % rows;
        m_takenAt = m_cursor;
        if (foreground)
            *foreground = true;
        return row;
    }

    // Walk forward from the cursor, wrapping at the end. Bounded by rows, so
    // this terminates even though m_remaining already guarantees a free slot
    // exists somewhere.
    for (int step = 0; step < rows; ++step) {
        const int row = (m_cursor + step) % rows;
        if (m_taken.testBit(row))
            continue;
        m_taken.setBit(row);
        --m_remaining;
        m_cursor = (row + 1) % rows;
        m_takenAt = m_cursor; // lets putBack tell "untouched" from "re-aimed"
        return row;
    }
    return -1; // unreachable while m_remaining > 0; defensive
}
