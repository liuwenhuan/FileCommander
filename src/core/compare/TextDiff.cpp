#include "TextDiff.h"

QVector<DiffLine> TextDiff::compare(const QStringList &leftLines, const QStringList &rightLines) {
    const int n = leftLines.size();
    const int m = rightLines.size();

    // dp[i][j] = length of the LCS of leftLines[i:] and rightLines[j:].
    QVector<QVector<int>> dp(n + 1, QVector<int>(m + 1, 0));
    for (int i = n - 1; i >= 0; --i) {
        for (int j = m - 1; j >= 0; --j) {
            if (leftLines.at(i) == rightLines.at(j))
                dp[i][j] = dp[i + 1][j + 1] + 1;
            else
                dp[i][j] = qMax(dp[i + 1][j], dp[i][j + 1]);
        }
    }

    QVector<DiffLine> result;
    int i = 0, j = 0;
    while (i < n && j < m) {
        if (leftLines.at(i) == rightLines.at(j)) {
            result.append({DiffLine::Kind::Same, leftLines.at(i), rightLines.at(j)});
            ++i;
            ++j;
        } else if (dp[i + 1][j] >= dp[i][j + 1]) {
            result.append({DiffLine::Kind::Removed, leftLines.at(i), QString()});
            ++i;
        } else {
            result.append({DiffLine::Kind::Added, QString(), rightLines.at(j)});
            ++j;
        }
    }
    while (i < n) {
        result.append({DiffLine::Kind::Removed, leftLines.at(i), QString()});
        ++i;
    }
    while (j < m) {
        result.append({DiffLine::Kind::Added, QString(), rightLines.at(j)});
        ++j;
    }
    return result;
}
