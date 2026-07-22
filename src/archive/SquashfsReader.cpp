#include "SquashfsReader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <cstring>

namespace {

const QString &unsquashfsExe() {
    static const QString exe = QStandardPaths::findExecutable(QStringLiteral("unsquashfs"));
    return exe;
}

const char kElfMagic[4] = {0x7f, 'E', 'L', 'F'};
// SquashFS little-endian superblock magic (0x73717368) as it appears on disk.
const char kSquashMagic[4] = {'h', 's', 'q', 's'};

quint16 rd16(const uchar *p, bool le) {
    return le ? quint16(p[0] | (p[1] << 8)) : quint16(p[1] | (p[0] << 8));
}
quint32 rd32(const uchar *p, bool le) {
    return le ? (quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16) |
                 (quint32(p[3]) << 24))
              : (quint32(p[3]) | (quint32(p[2]) << 8) | (quint32(p[1]) << 16) |
                 (quint32(p[0]) << 24));
}
quint64 rd64(const uchar *p, bool le) {
    quint64 v = 0;
    for (int i = 0; i < 8; ++i)
        v |= quint64(p[le ? i : 7 - i]) << (8 * i);
    return v;
}

// Parses an ELF header (>= 64 bytes) and returns the byte offset one past the
// section-header table -- where an appended filesystem (the AppImage squashfs)
// begins: e_shoff + e_shnum * e_shentsize. Handles ELFCLASS64 and ELFCLASS32 and
// either endianness. Returns -1 if the buffer isn't a valid ELF header.
qint64 elfEnd(const QByteArray &hdr) {
    if (hdr.size() < 24)
        return -1;
    const uchar *d = reinterpret_cast<const uchar *>(hdr.constData());
    if (std::memcmp(d, kElfMagic, 4) != 0)
        return -1;
    const int cls = d[4];        // EI_CLASS: 1=ELF32, 2=ELF64
    const bool le = d[5] != 2;   // EI_DATA:  1=LE, 2=BE
    if (cls == 2) {
        if (hdr.size() < 0x40)
            return -1;
        const quint64 shoff = rd64(d + 0x28, le);
        const quint16 shentsize = rd16(d + 0x3a, le);
        const quint16 shnum = rd16(d + 0x3c, le);
        return qint64(shoff + quint64(shentsize) * shnum);
    }
    if (cls == 1) {
        if (hdr.size() < 0x34)
            return -1;
        const quint32 shoff = rd32(d + 0x20, le);
        const quint16 shentsize = rd16(d + 0x2e, le);
        const quint16 shnum = rd16(d + 0x30, le);
        return qint64(shoff) + qint64(shentsize) * shnum;
    }
    return -1;
}

// Scans the file for the squashfs magic, returning its offset or -1. Only used as
// a fallback for files explicitly tagged AppImage type-2 (AI\x02 at offset 8), so
// arbitrary ELF executables are never fully scanned.
qint64 scanForSquash(QFile &f) {
    constexpr qint64 chunk = 1 << 20;
    const QByteArray needle(kSquashMagic, 4);
    qint64 base = 0;
    QByteArray prev;
    f.seek(0);
    for (;;) {
        const QByteArray buf = f.read(chunk);
        if (buf.isEmpty())
            break;
        const QByteArray window = prev + buf;
        const int idx = window.indexOf(needle);
        if (idx >= 0)
            return base - prev.size() + idx;
        base += buf.size();
        prev = buf.right(3); // carry the tail so a magic split across chunks still matches
    }
    return -1;
}

struct ProcResult {
    bool ran = false; // process actually finished (not killed / failed to start)
    int exitCode = -1;
    QByteArray out;
    QString err;
};

// Mirrors ExternalArchiveTool::runProcess: runs `exe args`, capturing stdout,
// polling `cancel` and enforcing a timeout so an abandoned preview can't wedge a
// worker thread. unsquashfs never prompts, so no stdin is needed.
ProcResult runProcess(const QString &exe, const QStringList &args, std::atomic<bool> *cancel,
                      int timeoutMs) {
    ProcResult r;
    QProcess proc;
    proc.setProgram(exe);
    proc.setArguments(args);
    proc.setProcessChannelMode(QProcess::SeparateChannels);
    proc.start(QIODevice::ReadOnly | QIODevice::Unbuffered);
    if (!proc.waitForStarted(5000))
        return r;

    int waited = 0;
    const int slice = 150;
    while (!proc.waitForFinished(slice)) {
        if (cancel && cancel->load()) {
            proc.kill();
            proc.waitForFinished(1000);
            return r;
        }
        waited += slice;
        if (waited >= timeoutMs) {
            proc.kill();
            proc.waitForFinished(1000);
            return r;
        }
    }
    r.ran = proc.exitStatus() == QProcess::NormalExit;
    r.exitCode = proc.exitCode();
    r.out = proc.readAllStandardOutput();
    r.err = QString::fromLocal8Bit(proc.readAllStandardError());
    return r;
}

// Strips unsquashfs's synthetic "squashfs-root" root component (and any leading
// slash) so entry paths are filesystem-root-relative, matching libarchive's
// convention. Returns "" for the root entry itself (caller skips it).
QString stripRoot(QString p) {
    p.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (p == QLatin1String("squashfs-root"))
        return {};
    if (p.startsWith(QLatin1String("squashfs-root/")))
        p = p.mid(14);
    else if (p.startsWith(QLatin1Char('/')))
        p = p.mid(1);
    while (p.endsWith(QLatin1Char('/')))
        p.chop(1);
    return p;
}

} // namespace

bool SquashfsReader::available() { return !unsquashfsExe().isEmpty(); }

qint64 SquashfsReader::squashfsOffset(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return -1;
    const QByteArray hdr = f.read(64);
    const qint64 end = elfEnd(hdr);
    if (end < 0)
        return -1;

    // Validate: the squashfs magic should sit exactly at the ELF's end.
    if (f.seek(end)) {
        const QByteArray m = f.read(4);
        if (m.size() == 4 && std::memcmp(m.constData(), kSquashMagic, 4) == 0)
            return end;
    }
    // Fallback: only for files flagged AppImage type-2 ('AI'\x02 at offset 8), scan
    // for the magic in case the computed offset was off (unusual toolchains).
    if (hdr.size() >= 11 && uchar(hdr[8]) == 0x41 && uchar(hdr[9]) == 0x49 &&
        uchar(hdr[10]) == 0x02)
        return scanForSquash(f);
    return -1;
}

bool SquashfsReader::isAppImage(const QString &path) { return squashfsOffset(path) >= 0; }

SquashfsReader::Status SquashfsReader::list(const QString &archivePath,
                                            const std::function<void(const Entry &)> &cb,
                                            std::atomic<bool> *cancel) {
    if (unsquashfsExe().isEmpty())
        return Status::Unavailable;
    const qint64 off = squashfsOffset(archivePath);
    if (off < 0)
        return Status::Error;

    const ProcResult r = runProcess(
        unsquashfsExe(),
        {QStringLiteral("-o"), QString::number(off), QStringLiteral("-ll"), archivePath}, cancel,
        60000);
    if (!r.ran || r.exitCode != 0)
        return Status::Error;

    // `-ll` emits `ls -l`-style lines: perms, owner/group, size, date, time, path
    // (symlinks add " -> target"). Fixed leading fields, then the path (which may
    // contain spaces), so anchor on the fixed columns and take the rest as the path.
    static const QRegularExpression re(QStringLiteral(
        "^([-dlbcps])[rwxsStT+-]{9}\\s+\\S+\\s+(\\d+)\\s+"
        "(\\d{4}-\\d{2}-\\d{2} \\d{2}:\\d{2}(?::\\d{2})?)\\s+(.+)$"));

    const QStringList lines = QString::fromUtf8(r.out).split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QRegularExpressionMatch m = re.match(line);
        if (!m.hasMatch())
            continue;
        const QChar type = m.captured(1).at(0);
        QString path = m.captured(4);
        const int arrow = path.indexOf(QStringLiteral(" -> "));
        if (arrow >= 0)
            path = path.left(arrow); // drop a symlink's target
        path = stripRoot(path);
        if (path.isEmpty())
            continue;

        Entry e;
        e.isDir = (type == QLatin1Char('d'));
        e.size = e.isDir ? 0 : m.captured(2).toLongLong();
        e.path = path;
        e.modified = QDateTime::fromString(m.captured(3).left(16),
                                           QStringLiteral("yyyy-MM-dd HH:mm"));
        cb(e);
    }
    return Status::Ok;
}

SquashfsReader::Status SquashfsReader::readEntry(const QString &archivePath,
                                                 const QString &entryPath, const QString &destPath,
                                                 std::atomic<bool> *cancel) {
    if (unsquashfsExe().isEmpty())
        return Status::Unavailable;
    const qint64 off = squashfsOffset(archivePath);
    if (off < 0)
        return Status::Error;

    QTemporaryDir tmp;
    if (!tmp.isValid())
        return Status::Error;

    QString rel = entryPath;
    while (rel.startsWith(QLatin1Char('/')))
        rel = rel.mid(1);
    if (rel.isEmpty())
        return Status::Error;

    // -f lets unsquashfs extract into the already-created temp dir; the entry lands
    // at <tmp>/<rel> (no squashfs-root prefix). Pass a leading '/' so the argument
    // is an absolute in-filesystem path.
    const ProcResult r = runProcess(
        unsquashfsExe(),
        {QStringLiteral("-o"), QString::number(off), QStringLiteral("-f"), QStringLiteral("-d"),
         tmp.path(), archivePath, QLatin1Char('/') + rel},
        cancel, 120000);
    if (!r.ran || r.exitCode != 0)
        return Status::Error;

    const QString extracted = QDir(tmp.path()).filePath(rel);
    const QFileInfo fi(extracted);
    if (!fi.exists() || fi.isDir())
        return Status::Error;

    QDir().mkpath(QFileInfo(destPath).absolutePath());
    QFile::remove(destPath); // QFile::copy refuses to overwrite an existing file
    if (!QFile::copy(extracted, destPath))
        return Status::Error;
    return Status::Ok;
}
