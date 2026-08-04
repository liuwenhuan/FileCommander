#include "ArchiveVolumes.h"

#include <QFileInfo>
#include <QRegularExpression>

namespace {

// name.partNN.<ext> -- RAR3 and later, and what 7-Zip writes for -v on rar.
// The digit width is part of the set's identity: a `part007` set's first volume
// is `part001`, not `part1`.
const QRegularExpression &partPattern() {
    static const QRegularExpression re(
        QStringLiteral("^(?<stem>.*)\\.part(?<num>\\d+)\\.(?<ext>rar|exe|sfx)$"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

// name.rNN -- the old RAR convention, where the FIRST volume is name.rar and
// the continuations are .r00, .r01, ... (0-based, so .rNN is volume NN+2).
const QRegularExpression &oldRarPattern() {
    static const QRegularExpression re(QStringLiteral("^(?<stem>.*)\\.r(?<num>\\d{2,})$"),
                                       QRegularExpression::CaseInsensitiveOption);
    return re;
}

// name.<anything>.NNN -- 7-Zip's raw byte split, first volume .001. Three
// digits at least, so a `.7z.1` typo is not mistaken for one.
const QRegularExpression &splitPattern() {
    static const QRegularExpression re(QStringLiteral("^(?<stem>.*)\\.(?<num>\\d{3,})$"));
    return re;
}

QString sameDirectory(const QString &path, const QString &fileName) {
    return QFileInfo(path).absolutePath() + QLatin1Char('/') + fileName;
}

} // namespace

namespace fc {

bool isVolumeMember(const QString &path) {
    const QString name = QFileInfo(path).fileName();
    if (partPattern().match(name).hasMatch())
        return true;
    if (splitPattern().match(name).hasMatch())
        return true;
    if (oldRarPattern().match(name).hasMatch())
        return true;
    // `name.rar` is only a volume member when a `.r00` sibling says the set
    // exists. On its own it is an ordinary archive, and treating every .rar as
    // a volume would route them all through the external tool for nothing.
    if (name.endsWith(QStringLiteral(".rar"), Qt::CaseInsensitive)) {
        const QString stem = name.left(name.size() - 4);
        return QFileInfo::exists(sameDirectory(path, stem + QStringLiteral(".r00")));
    }
    return false;
}

QString firstVolumeOf(const QString &path) {
    const QFileInfo info(path);
    const QString name = info.fileName();

    if (const auto match = partPattern().match(name); match.hasMatch()) {
        const QString stem = match.captured(QStringLiteral("stem"));
        const QString num = match.captured(QStringLiteral("num"));
        // "1" padded to the width this set uses: part01, part001, ...
        QString first = QStringLiteral("1").rightJustified(num.size(), QLatin1Char('0'));
        // The first volume's extension is NOT the member's -- it is routinely a
        // self-extracting .exe while the rest are .rar. Probe rather than guess.
        for (const QString &ext : {QStringLiteral("rar"), QStringLiteral("exe"),
                                   QStringLiteral("sfx"), match.captured(QStringLiteral("ext"))}) {
            const QString candidate =
                sameDirectory(path, QStringLiteral("%1.part%2.%3").arg(stem, first, ext));
            if (QFileInfo::exists(candidate))
                return candidate;
        }
        return {};
    }

    if (const auto match = splitPattern().match(name); match.hasMatch()) {
        const QString stem = match.captured(QStringLiteral("stem"));
        const QString num = match.captured(QStringLiteral("num"));
        const QString first = QStringLiteral("1").rightJustified(num.size(), QLatin1Char('0'));
        const QString candidate = sameDirectory(path, stem + QLatin1Char('.') + first);
        return QFileInfo::exists(candidate) ? candidate : QString();
    }

    if (const auto match = oldRarPattern().match(name); match.hasMatch()) {
        // .r00 is ambiguous: it is a continuation when a `.rar` exists beside
        // it, and the set's own first volume when one does not. Probing
        // resolves it without having to guess.
        const QString stem = match.captured(QStringLiteral("stem"));
        const QString candidate = sameDirectory(path, stem + QStringLiteral(".rar"));
        if (QFileInfo::exists(candidate))
            return candidate;
        return match.captured(QStringLiteral("num")) == QStringLiteral("00") ? path : QString();
    }

    // A plain `.rar` that has continuations IS the first volume.
    if (name.endsWith(QStringLiteral(".rar"), Qt::CaseInsensitive) && isVolumeMember(path))
        return path;
    return {};
}

bool isRawSplit(const QString &firstVolume) {
    return splitPattern().match(QFileInfo(firstVolume).fileName()).hasMatch();
}

QStringList volumeChain(const QString &firstVolume) {
    const QFileInfo info(firstVolume);
    const QString name = info.fileName();
    QStringList chain;

    // Raw split: name.<n>, zero-padded to the width the first volume uses.
    if (const auto match = splitPattern().match(name); match.hasMatch()) {
        const QString stem = match.captured(QStringLiteral("stem"));
        const int width = match.captured(QStringLiteral("num")).size();
        for (int n = 1;; ++n) {
            const QString candidate = sameDirectory(
                firstVolume,
                QStringLiteral("%1.%2").arg(
                    stem, QString::number(n).rightJustified(width, QLatin1Char('0'))));
            if (!QFileInfo::exists(candidate))
                break;
            chain << candidate;
        }
        return chain;
    }

    // name.partNN.<ext>: the first volume's extension is its own (it is often
    // an SFX .exe), the rest are what the member that led us here used.
    if (const auto match = partPattern().match(name); match.hasMatch()) {
        const QString stem = match.captured(QStringLiteral("stem"));
        const int width = match.captured(QStringLiteral("num")).size();
        chain << firstVolume;
        for (int n = 2;; ++n) {
            const QString number = QString::number(n).rightJustified(width, QLatin1Char('0'));
            QString found;
            for (const QString &ext : {QStringLiteral("rar"), QStringLiteral("exe")}) {
                const QString candidate = sameDirectory(
                    firstVolume, QStringLiteral("%1.part%2.%3").arg(stem, number, ext));
                if (QFileInfo::exists(candidate)) {
                    found = candidate;
                    break;
                }
            }
            if (found.isEmpty())
                break;
            chain << found;
        }
        return chain;
    }

    // name.rar + name.r00, name.r01, ...
    if (name.endsWith(QStringLiteral(".rar"), Qt::CaseInsensitive)) {
        const QString stem = name.left(name.size() - 4);
        chain << firstVolume;
        for (int n = 0;; ++n) {
            const QString candidate = sameDirectory(
                firstVolume, QStringLiteral("%1.r%2").arg(
                                 stem, QString::number(n).rightJustified(2, QLatin1Char('0'))));
            if (!QFileInfo::exists(candidate))
                break;
            chain << candidate;
        }
        return chain;
    }

    if (QFileInfo::exists(firstVolume))
        chain << firstVolume;
    return chain;
}

} // namespace fc
