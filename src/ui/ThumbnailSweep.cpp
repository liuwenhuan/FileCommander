#include "ThumbnailSweep.h"

void ThumbnailSweep::reset(int rowCount) {
    const int rows = rowCount > 0 ? rowCount : 0;
    m_taken = QBitArray(rows);
    m_cursor = 0;
    m_takenAt = 0;
    m_remaining = rows;
}

void ThumbnailSweep::focusOn(int firstVisible, int lastVisible) {
    Q_UNUSED(lastVisible);
    const int rows = m_taken.size();
    if (rows == 0)
        return;
    m_cursor = qBound(0, firstVisible, rows - 1);
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
    if (m_cursor == m_takenAt)
        m_cursor = row;
}

int ThumbnailSweep::next() {
    const int rows = m_taken.size();
    if (rows == 0 || m_remaining <= 0)
        return -1;

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
