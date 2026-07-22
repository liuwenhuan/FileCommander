#include "SquashfsReader.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QVector>

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

// Actual offset computation (uncached). Returns the squashfs offset, or -1.
qint64 computeSquashfsOffset(const QString &path) {
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

// Cache of computed offsets keyed by path, invalidated when the file's mtime or
// size changes. Guards against a full magic scan on every recognition call (e.g.
// ArchiveHandler::isSupportedArchive runs on every QuickView selection change).
struct OffsetCacheEntry {
    qint64 mtime = 0;
    qint64 size = -1;
    qint64 offset = -1;
};
QHash<QString, OffsetCacheEntry> g_offsetCache;
QMutex g_offsetMutex;

// unsquashfs >= 4.5 sanitises pathnames on extraction (CVE-2021-40153/41072), so
// a crafted `..` name can't write outside the destination during a bare
// extract-all. Query and cache the installed version once; if it's older (or
// unparseable) we won't trust a bare extract-all and fall back to validated
// per-entry extraction instead.
bool computeUnsquashfsAtLeast45() {
    if (unsquashfsExe().isEmpty())
        return false;
    const ProcResult r = runProcess(unsquashfsExe(), {QStringLiteral("-version")}, nullptr, 5000);
    // -version historically exits non-zero; parse whatever it printed regardless.
    const QString text = QString::fromLocal8Bit(r.out) + QLatin1Char('\n') + r.err;
    static const QRegularExpression re(QStringLiteral("version\\s+(\\d+)\\.(\\d+)"));
    const QRegularExpressionMatch m = re.match(text);
    if (!m.hasMatch())
        return false; // unknown -> treat as unsafe
    const int major = m.captured(1).toInt();
    const int minor = m.captured(2).toInt();
    return major > 4 || (major == 4 && minor >= 5);
}

bool unsquashfsAtLeast45() {
    static const bool ok = computeUnsquashfsAtLeast45();
    return ok;
}

// SECURITY: after an extraction, remove any symlink in `destDir` whose target
// escapes `destDir`. unsquashfs recreates symlinks faithfully, so a crafted entry
// like `secret.txt -> /home/<user>/.ssh/id_rsa` would otherwise sit live in the
// user's chosen folder and disclose the target when opened. Legitimate internal
// relative links (e.g. usr/lib/libfoo.so -> libfoo.so.1) resolve within destDir
// and are kept. Returns the number of escaping links removed.
int stripEscapingSymlinks(const QString &destDir) {
    const QString base = QDir(destDir).canonicalPath();
    if (base.isEmpty())
        return 0;
    int removed = 0;
    // Do NOT follow symlinks while iterating (no FollowSymlinks) so we can't be led
    // outside; each symlink entry is inspected in place.
    QDirIterator it(destDir,
                    QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        const QFileInfo fi = it.fileInfo();
        if (!fi.isSymLink())
            continue;
        // Where does it point? Prefer the fully-resolved target (exists); fall back
        // to the raw (possibly dangling) target string for broken/outside links.
        QString resolved = fi.canonicalFilePath();
        if (resolved.isEmpty())
            resolved = QDir::cleanPath(fi.symLinkTarget());
        const bool inside = resolved == base || resolved.startsWith(base + QLatin1Char('/'));
        if (!inside) {
            if (QFile::remove(path))
                ++removed;
        }
    }
    return removed;
}

} // namespace

bool SquashfsReader::available() { return !unsquashfsExe().isEmpty(); }

qint64 SquashfsReader::squashfsOffset(const QString &path) {
    const QFileInfo fi(path);
    if (!fi.isFile())
        return -1;
    const qint64 mtime = fi.lastModified().toMSecsSinceEpoch();
    const qint64 size = fi.size();
    {
        QMutexLocker lock(&g_offsetMutex);
        const auto it = g_offsetCache.constFind(path);
        if (it != g_offsetCache.constEnd() && it->mtime == mtime && it->size == size)
            return it->offset; // may be -1 (cached "not an AppImage")
    }
    const qint64 off = computeSquashfsOffset(path);
    {
        QMutexLocker lock(&g_offsetMutex);
        g_offsetCache.insert(path, OffsetCacheEntry{mtime, size, off});
    }
    return off;
}

bool SquashfsReader::isAppImage(const QString &path) { return squashfsOffset(path) >= 0; }

bool SquashfsReader::isSafeEntryPath(const QString &path) {
    if (path.isEmpty())
        return false;
    QString p = path;
    p.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (p.startsWith(QLatin1Char('/')))
        return false; // absolute
    const QStringList parts = p.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    for (const QString &c : parts) {
        if (c.isEmpty() || c == QLatin1String(".") || c == QLatin1String(".."))
            return false;
    }
    return true;
}

bool SquashfsReader::isContained(const QString &baseDir, const QString &relPath) {
    const QString base = QDir::cleanPath(QDir(baseDir).absolutePath());
    const QString target = QDir::cleanPath(QDir(base).absoluteFilePath(relPath));
    return target == base || target.startsWith(base + QLatin1Char('/'));
}

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
        const bool isSymlink = (type == QLatin1Char('l'));
        QString path = m.captured(4);
        if (isSymlink) {
            // Only a symlink line carries " -> target"; strip it. (A regular file
            // literally named "a -> b" must NOT be truncated.)
            const int arrow = path.indexOf(QStringLiteral(" -> "));
            if (arrow >= 0)
                path = path.left(arrow);
        }
        path = stripRoot(path);
        if (path.isEmpty())
            continue;
        // SECURITY: drop attacker-controlled names that could escape the dest dir.
        if (!isSafeEntryPath(path))
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

    // SECURITY: reject traversal in the (attacker-controlled) entry name before it
    // is ever joined onto a real directory. `rel` is relative with no '.'/'..'.
    QString rel = entryPath;
    while (rel.startsWith(QLatin1Char('/')))
        rel = rel.mid(1);
    if (!isSafeEntryPath(rel))
        return Status::Error;

    const qint64 off = squashfsOffset(archivePath);
    if (off < 0)
        return Status::Error;

    QTemporaryDir tmp;
    if (!tmp.isValid())
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

    // Defense in depth: `rel` is validated, but re-check the resolved target stays
    // inside the temp dir before we read it back.
    if (!isContained(tmp.path(), rel))
        return Status::Error;
    const QString extracted = QDir(tmp.path()).filePath(rel);
    const QFileInfo fi(extracted);
    // SECURITY: refuse symlinks -- unsquashfs recreates them faithfully, and a link
    // (e.g. -> /etc/passwd) would otherwise be followed by QFile::copy, disclosing a
    // file outside the archive. Also refuse dirs / missing entries.
    if (!fi.exists() || fi.isDir() || fi.isSymLink())
        return Status::Error;

    QDir().mkpath(QFileInfo(destPath).absolutePath());
    QFile::remove(destPath); // QFile::copy refuses to overwrite an existing file
    if (!QFile::copy(extracted, destPath))
        return Status::Error;
    return Status::Ok;
}

SquashfsReader::Status SquashfsReader::extractTo(const QString &archivePath,
                                                 const QStringList &entries, const QString &destDir,
                                                 std::atomic<bool> *cancel) {
    if (unsquashfsExe().isEmpty())
        return Status::Unavailable;
    const qint64 off = squashfsOffset(archivePath);
    if (off < 0)
        return Status::Error;

    QDir().mkpath(destDir);
    const bool selective = !entries.isEmpty();

    // Bare extract-all (no explicit names) trusts unsquashfs to sanitise '..' in
    // stored names. Only >= 4.5 does; on an older/unknown build, extract entry by
    // entry through readEntry() -- which validates each name and rejects symlinks in
    // app -- instead of trusting the tool.
    if (!selective && !unsquashfsAtLeast45()) {
        QVector<Entry> files;
        const Status ls = list(archivePath, [&](const Entry &e) {
            if (!e.isDir)
                files.append(e);
        }, cancel);
        if (ls != Status::Ok)
            return ls;
        bool ok = true;
        for (const Entry &e : files) {
            const QString destPath = QDir(destDir).filePath(e.path);
            if (readEntry(archivePath, e.path, destPath, cancel) != Status::Ok)
                ok = false; // e.g. a symlink entry -- skipped, not written
        }
        return ok ? Status::Ok : Status::Error;
    }

    QStringList args = {QStringLiteral("-o"), QString::number(off), QStringLiteral("-f"),
                        QStringLiteral("-d"), destDir, archivePath};

    // Selective extraction: pass only the validated entry paths, so unsquashfs never
    // touches an unsafe name. Extraction is a SINGLE process for the whole set (not
    // one process per file). An empty selection means "everything".
    int added = 0;
    for (const QString &e : entries) {
        QString rel = e;
        while (rel.startsWith(QLatin1Char('/')))
            rel = rel.mid(1);
        if (!isSafeEntryPath(rel))
            continue; // drop unsafe selection entries
        args << (QLatin1Char('/') + rel);
        ++added;
    }
    // A selection that was entirely unsafe must NOT collapse into an extract-all;
    // report it as an error rather than a silent success.
    if (selective && added == 0)
        return Status::Error;

    const ProcResult r = runProcess(unsquashfsExe(), args, cancel, 600000);
    if (!r.ran || r.exitCode != 0)
        return Status::Error;

    // SECURITY (authoritative guard): unsquashfs faithfully recreates symlinks, so
    // an entry pointing outside destDir (absolute, or relative via '..') would sit
    // live in the user's folder. Remove any such escaping link; keep internal ones.
    stripEscapingSymlinks(destDir);
    return Status::Ok;
}
