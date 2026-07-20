#include "FileSplitter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

namespace {
constexpr qint64 kBufferSize = 1 << 20; // 1 MiB streaming buffer

QString partPath(const QString &destDir, const QString &base, int index) {
    return QDir(destDir).filePath(QStringLiteral("%1.%2").arg(base).arg(index, 3, 10,
                                                                       QLatin1Char('0')));
}
} // namespace

QStringList FileSplitter::split(const QString &sourcePath, qint64 partSize, const QString &destDir,
                                 QString *errorMessage) {
    auto fail = [&](const QString &msg) -> QStringList {
        if (errorMessage)
            *errorMessage = msg;
        return {};
    };

    if (partSize <= 0)
        return fail(QStringLiteral("Part size must be positive"));

    QFile in(sourcePath);
    if (!in.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("Cannot open %1").arg(sourcePath));

    QDir().mkpath(destDir);
    const QString base = QFileInfo(sourcePath).fileName();
    QStringList parts;
    int index = 1;

    while (!in.atEnd()) {
        const QString path = partPath(destDir, base, index);
        QFile out(path);
        if (!out.open(QIODevice::WriteOnly))
            return fail(QStringLiteral("Cannot write %1").arg(path));

        qint64 written = 0;
        while (written < partSize && !in.atEnd()) {
            const QByteArray chunk = in.read(qMin(kBufferSize, partSize - written));
            if (chunk.isEmpty())
                break;
            if (out.write(chunk) != chunk.size())
                return fail(QStringLiteral("Write failed for %1").arg(path));
            written += chunk.size();
        }
        out.close();
        parts << path;
        ++index;
    }
    return parts;
}

QString FileSplitter::baseNameForPart(const QString &partPath) {
    const QString name = QFileInfo(partPath).fileName();
    static const QRegularExpression suffix(QStringLiteral("\\.\\d{3,}$"));
    const QRegularExpressionMatch m = suffix.match(name);
    if (!m.hasMatch())
        return {};
    return name.left(m.capturedStart());
}

bool FileSplitter::merge(const QString &firstPartPath, const QString &destPath,
                          QString *errorMessage) {
    auto fail = [&](const QString &msg) {
        if (errorMessage)
            *errorMessage = msg;
        return false;
    };

    const QFileInfo firstInfo(firstPartPath);
    const QString base = baseNameForPart(firstPartPath);
    if (base.isEmpty())
        return fail(QStringLiteral("%1 is not a numbered part").arg(firstPartPath));
    const QString dir = firstInfo.absolutePath();

    QFile out(destPath);
    if (!out.open(QIODevice::WriteOnly))
        return fail(QStringLiteral("Cannot write %1").arg(destPath));

    for (int index = 1;; ++index) {
        const QString path = partPath(dir, base, index);
        if (!QFileInfo::exists(path))
            break;
        QFile part(path);
        if (!part.open(QIODevice::ReadOnly))
            return fail(QStringLiteral("Cannot open %1").arg(path));
        while (!part.atEnd()) {
            const QByteArray chunk = part.read(kBufferSize);
            if (out.write(chunk) != chunk.size())
                return fail(QStringLiteral("Write failed for %1").arg(destPath));
        }
    }
    return true;
}
