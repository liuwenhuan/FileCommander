#pragma once

#include <QVector>

#include "FramelessDialog.h"
#include "TextDiff.h"

class QPlainTextEdit;
class QLabel;

// Side-by-side line diff of two text files (Commands > Compare by
// Content...). Gaps are inserted on whichever side is missing a line so
// matching/differing lines stay vertically aligned, and scrolling is
// synchronized between the two panes.
class CompareDialog : public FramelessDialog {
    Q_OBJECT

public:
    CompareDialog(const QString &leftPath, const QString &rightPath, QWidget *parent = nullptr);
    // Same, but labelled with something other than the paths it reads. A file on
    // a share is diffed through its gvfs mount point or a temp copy, and naming
    // /run/user/1000/gvfs/... (or /tmp/FileCommander-open-7/notes.txt) in the
    // header would tell the user nothing about which file they are looking at.
    CompareDialog(const QString &leftPath, const QString &rightPath, const QString &leftLabel,
                  const QString &rightLabel, QWidget *parent = nullptr);

private:
    static constexpr qint64 kMaxCompareBytes = 2 * 1024 * 1024; // 2 MB
    static constexpr int kMaxCompareLines = 5000;

    // Reading both files and diffing them is bounded but not cheap, and it ran
    // before the dialog had painted once. Split so the expensive half can go on
    // a worker: compareFiles() touches no widget, applyComparison() only fills
    // them in.
    struct CompareResult {
        QVector<DiffLine> diff;
        QString error;
    };
    static CompareResult compareFiles(const QString &leftPath, const QString &rightPath);
    void applyComparison(const CompareResult &result);

    QPlainTextEdit *m_leftEdit;
    QPlainTextEdit *m_rightEdit;
    QLabel *m_summaryLabel;
    bool m_syncing = false;
};
