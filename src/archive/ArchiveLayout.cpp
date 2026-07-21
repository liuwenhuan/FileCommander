#include "ArchiveLayout.h"

#include <QVector>

namespace ArchiveLayout {

namespace {

// Normalises one raw entry path: backslashes -> '/', records whether it was an
// explicit directory entry (trailing '/'), then trims trailing slashes.
struct NormEntry {
    QString path;    // no leading/trailing slashes, '/'-separated
    bool explicitDir = false;
};

NormEntry normalise(const QString &raw) {
    NormEntry e;
    QString p = raw;
    p.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (p.startsWith(QLatin1Char('/')))
        p.remove(0, 1);
    if (p.endsWith(QLatin1Char('/')))
        e.explicitDir = true;
    while (p.endsWith(QLatin1Char('/')))
        p.chop(1);
    e.path = p;
    return e;
}

} // namespace

bool hasArchiveSuffix(const QString &name) {
    // Ordered longest-first so ".tar.gz" wins over ".gz" (not that it matters
    // for a boolean, but keeps the intent clear).
    static const QStringList kSuffixes = {
        QStringLiteral(".tar.gz"),  QStringLiteral(".tar.bz2"), QStringLiteral(".tar.xz"),
        QStringLiteral(".tgz"),     QStringLiteral(".tbz2"),    QStringLiteral(".txz"),
        QStringLiteral(".zip"),     QStringLiteral(".7z"),      QStringLiteral(".rar"),
        QStringLiteral(".tar"),     QStringLiteral(".gz"),      QStringLiteral(".bz2"),
        QStringLiteral(".xz"),
    };
    const QString lower = name.toLower();
    for (const QString &s : kSuffixes) {
        if (lower.endsWith(s))
            return true;
    }
    return false;
}

Result analyze(const QStringList &entryPaths, const QString & /*archiveBaseName*/) {
    Result result;

    // First pass: collect distinct top-level segments (in first-seen order) and
    // whether each is a directory (has children, or is an explicit dir entry).
    QStringList topOrder;
    QVector<bool> topIsDir;
    auto topIndex = [&](const QString &seg) -> int {
        const int idx = topOrder.indexOf(seg);
        if (idx >= 0)
            return idx;
        topOrder.append(seg);
        topIsDir.append(false);
        return topOrder.size() - 1;
    };

    for (const QString &raw : entryPaths) {
        const NormEntry e = normalise(raw);
        if (e.path.isEmpty())
            continue;
        const QStringList parts = e.path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (parts.isEmpty())
            continue;
        const int idx = topIndex(parts.first());
        // A top-level segment is a directory if any entry nests under it, or the
        // entry that names it was an explicit directory entry.
        if (parts.size() > 1 || e.explicitDir)
            topIsDir[idx] = true;
    }

    if (topOrder.isEmpty())
        return result; // empty archive: nothing to do

    // Helper computing the "effective" top-level items after any strip, and
    // whether that set is exactly one archive file.
    auto detectSingleArchive = [&](const QStringList &names, const QVector<bool> &isDir) {
        if (names.size() == 1 && !isDir.first() && hasArchiveSuffix(names.first())) {
            result.resultIsSingleArchive = true;
            result.innerArchiveName = names.first();
        }
    };

    if (topOrder.size() == 1) {
        const QString single = topOrder.first();
        if (topIsDir.first()) {
            // Single common top-level directory -> strip it. Recompute the direct
            // children of that directory to detect a lone inner archive.
            result.stripSingleRoot = true;
            result.strippedPrefix = single;

            const QString prefix = single + QLatin1Char('/');
            QStringList childOrder;
            QVector<bool> childIsDir;
            auto childIndex = [&](const QString &seg) -> int {
                const int idx = childOrder.indexOf(seg);
                if (idx >= 0)
                    return idx;
                childOrder.append(seg);
                childIsDir.append(false);
                return childOrder.size() - 1;
            };
            for (const QString &raw : entryPaths) {
                const NormEntry e = normalise(raw);
                if (!e.path.startsWith(prefix))
                    continue;
                const QString rest = e.path.mid(prefix.size());
                const QStringList parts = rest.split(QLatin1Char('/'), Qt::SkipEmptyParts);
                if (parts.isEmpty())
                    continue;
                const int idx = childIndex(parts.first());
                if (parts.size() > 1 || e.explicitDir)
                    childIsDir[idx] = true;
            }
            detectSingleArchive(childOrder, childIsDir);
        } else {
            // A single file at the root: extract directly. Nothing to strip or
            // wrap; it may itself be an archive (nested).
            detectSingleArchive(topOrder, topIsDir);
        }
    } else {
        // Multiple top-level items -> wrap under an archive-named folder.
        result.wrapInArchiveNamedFolder = true;
    }

    return result;
}

} // namespace ArchiveLayout
