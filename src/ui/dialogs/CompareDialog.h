#pragma once

#include "FramelessDialog.h"

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

private:
    static constexpr qint64 kMaxCompareBytes = 2 * 1024 * 1024; // 2 MB
    static constexpr int kMaxCompareLines = 5000;

    bool loadAndCompare(const QString &leftPath, const QString &rightPath, QString *errorMessage);

    QPlainTextEdit *m_leftEdit;
    QPlainTextEdit *m_rightEdit;
    QLabel *m_summaryLabel;
    bool m_syncing = false;
};
