#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace fc {

// One way of opening a terminal: the program to run and the arguments it needs
// to start in a particular directory. Most terminals simply inherit the working
// directory the process is started with and need no arguments; the ones that do
// not are exactly why this carries arguments at all.
struct TerminalCandidate {
    QString program;
    QStringList arguments;
};

// The terminals worth trying on this platform, best first. Ordered by what a
// user would expect to get, not by what is most likely to exist -- the caller
// walks the list and takes the first one actually installed.
//
// `workingDirectory` is baked into the arguments where a terminal needs to be
// told explicitly rather than inheriting it.
QVector<TerminalCandidate> terminalCandidates(const QString &workingDirectory);

// Starts the first candidate that is both installed and launches successfully.
// Returns false when none of them is -- distinguishing "not installed" from
// "refused to start" is not something the caller can act on differently, but
// trying the next one instead of giving up on the first failure is.
bool openTerminalAt(const QString &workingDirectory);

} // namespace fc
