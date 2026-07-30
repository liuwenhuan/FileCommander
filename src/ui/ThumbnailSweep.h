#pragma once

#include <QBitArray>
#include <QVector>

// Decides the order in which a listing's thumbnails are fetched, so a network
// directory fills in completely instead of stopping at whatever happened to be
// on screen.
//
// The foreground order serves visible rows, then up to two viewport heights on
// each side. The remaining rows use the regular wrap-around scan and can be
// persisted without spending decoded-pixmap memory.
//
// Rows already taken are remembered, so refocusing costs nothing: when the user
// scrolls, focusOn() simply moves the cursor to the new visible range and the
// scan continues from there, skipping everything already done. Work is never
// repeated and never lost -- rows passed over during a scroll are still picked
// up when the scan wraps around to them.
//
// The preemption this gives is approximate, not absolute: focusOn() only steers
// what is handed out NEXT. Requests already submitted to the fetcher keep
// running in submission order (see kMaxOutstanding in RemoteThumbnailFetcher.cpp
// for why that is bounded and deliberate), so after a scroll the newly visible
// rows wait behind the shallow queue of already-submitted ones.
//
// Pure bookkeeping: no Qt objects, no I/O, no knowledge of what a "thumbnail"
// is. The caller pops rows, does the actual work, and reports completion.
class ThumbnailSweep {
public:
    // Starts a fresh sweep over `rowCount` rows with nothing done yet. Called
    // whenever the listing changes (new directory, refresh, filter).
    void reset(int rowCount);

    // Moves the cursor to `firstVisible`, so the next rows handed out are the
    // ones on screen. Rows already done stay done. Out-of-range values are
    // clamped; `lastVisible` is accepted for symmetry with the view's signal
    // but the scan needs only the start (it covers the whole listing anyway).
    void focusOn(int firstVisible, int lastVisible);

    // Puts visible rows first, followed by up to two viewport heights after
    // and before them. The caller can mark this foreground band as display
    // work while the rest of the directory continues as persistence-only.
    void focusVisibleRowsWithAdjacentViewports(int firstVisible, int lastVisible);

    // The next row to fetch, or -1 when every row has been taken. `foreground`
    // reports whether the row is visible or near-visible display work; all
    // other rows belong to the persistence-only directory sweep.
    int next(bool *foreground = nullptr);

    // Returns a row taken by next() to the pool, so it is handed out again.
    // Normally the cursor rewinds to it -- the caller only puts a row back
    // because it could not be placed right now, so it is still the most urgent
    // one. But a focusOn() since that next() means the user has scrolled
    // elsewhere, and the new position wins: the row is still freed (never
    // lost -- the scan reaches it on the way round) but does not jump the
    // queue ahead of what is now on screen. No-op for a row never taken.
    void putBack(int row);

    // True once every row has been handed out.
    bool complete() const { return m_remaining <= 0; }

    // Rows not yet handed out. Exposed for progress reporting and tests.
    int remaining() const { return m_remaining; }
    int rowCount() const { return m_taken.size(); }

private:
    QBitArray m_taken;   // per-row: already handed out by next()
    int m_cursor = 0;    // where the scan resumes
    int m_takenAt = 0;   // m_cursor as next() left it; putBack compares to spot a focusOn
    int m_remaining = 0; // rows still to hand out
    QVector<int> m_foregroundRows;
    int m_nextForegroundRow = 0;
};
