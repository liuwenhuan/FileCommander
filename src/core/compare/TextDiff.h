#pragma once

#include <QStringList>
#include <QVector>

struct DiffLine {
    enum class Kind { Same, Added, Removed };
    Kind kind;
    QString leftText;  // empty when kind == Added
    QString rightText; // empty when kind == Removed
};

// Classic LCS-based line diff (same structure as a basic `diff`, without
// change-pair merging -- an adjacent Removed+Added pair renders as two
// lines rather than one "changed" line, which is simpler and still
// perfectly readable for the side-by-side view this backs).
class TextDiff {
public:
    // Deliberately O(n*m) time/space (a straightforward LCS table) -- fine
    // for the file sizes a manual side-by-side compare is actually used
    // for; callers should cap input size for very large files (see
    // CompareDialog's line-count cap).
    static QVector<DiffLine> compare(const QStringList &leftLines, const QStringList &rightLines);
};
