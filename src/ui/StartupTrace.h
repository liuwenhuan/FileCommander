#pragma once

#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>

// When each step of startup finished, in the order the steps happened.
//
// This was nineteen `qint64 m_startupSomethingMs = -1;` members, nineteen
// `if (m_collectStartupPhases) m_startupSomethingMs = elapsedSinceStartup();`
// lines, and nineteen `metrics.insert("somethingMs", m_startupSomethingMs)`
// lines -- the same names written out three times, in three places that had to
// agree. Adding a phase meant editing all three, and the only thing that
// noticed a mistake was the probe script refusing the output.
//
// Deliberately owns no clock. The caller passes the time, because the caller
// already has one -- MainWindow::elapsedSinceStartup(), which prefers the real
// clock handed down from main() over anything measured after the window
// existed. A timer of this class's own looked tidier and was wrong: it started
// when the window did and lost the ~50 ms between main() taking its reading and
// the constructor running, which then reappeared attributed to a later phase.
//
// The order is insertion order rather than anything sorted: the probe checks
// that the timings never go backwards, and that check only means something
// against the order the phases actually ran in.
class StartupTrace {
public:
    // Startup does real work to collect these, so a normal run does not: only
    // --startup-probe turns it on.
    void setCollecting(bool collecting) { m_collecting = collecting; }
    bool isCollecting() const { return m_collecting; }

    // Records that `name` finished at `elapsedMs` after process start. Ignored
    // when not collecting, so call sites need no condition of their own.
    void mark(const QString &name, qint64 elapsedMs);

    // The recorded phases, in order, as name -> milliseconds.
    QJsonObject toJson() const;
    bool isEmpty() const { return m_phases.isEmpty(); }

private:
    bool m_collecting = false;
    QList<QPair<QString, qint64>> m_phases;
};
