#include "PackageInfo.h"

#include "ArchiveNames.h"

#include <archive.h>
#include <archive_entry.h>

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QMap>

namespace {

// Read the current libarchive entry's data into a QByteArray.
QByteArray readEntryData(struct archive *a) {
    QByteArray out;
    const void *buff = nullptr;
    size_t len = 0;
    la_int64_t offset = 0;
    int r;
    while ((r = archive_read_data_block(a, &buff, &len, &offset)) == ARCHIVE_OK)
        out.append(static_cast<const char *>(buff), static_cast<int>(len));
    if (r != ARCHIVE_EOF && out.isEmpty())
        return QByteArray();
    return out;
}

// Extract one member (first whose base name / prefix matches `matcher`) from an
// archive opened by `opener`, returning its bytes. Generic over on-disk (.deb)
// and in-memory (control.tar.*) sources.
template <typename Opener, typename Matcher>
QByteArray extractMember(Opener opener, Matcher matcher) {
    struct archive *a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    fc::applyHeaderCharset(a);
    if (!opener(a)) {
        archive_read_free(a);
        return QByteArray();
    }
    QByteArray result;
    struct archive_entry *entry = nullptr;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        const QString name = fc::entryPathname(entry);
        if (matcher(name)) {
            result = readEntryData(a);
            break;
        }
        archive_read_data_skip(a);
    }
    archive_read_free(a);
    return result;
}

QString readDebControl(const QString &path) {
    // .deb is an `ar` archive whose control.tar.{gz,xz,zst,...} holds `control`.
    const QByteArray localPath = QFile::encodeName(path);
    const QByteArray controlTar = extractMember(
        [&](struct archive *a) {
            return archive_read_open_filename(a, localPath.constData(), 65536) ==
                   ARCHIVE_OK;
        },
        [](const QString &name) { return name.startsWith(QLatin1String("control.tar")); });
    if (controlTar.isEmpty())
        return QString();

    const QByteArray control = extractMember(
        [&](struct archive *a) {
            return archive_read_open_memory(a, controlTar.constData(),
                                            static_cast<size_t>(controlTar.size())) ==
                   ARCHIVE_OK;
        },
        [](const QString &name) {
            // Entries are usually "./control"; match the base name defensively.
            return name.section(QLatin1Char('/'), -1) == QLatin1String("control");
        });
    if (control.isEmpty())
        return QString();

    return QString::fromUtf8(control).trimmed();
}

// --- Minimal RPM header parser ---------------------------------------------
// RPM layout: 96-byte lead, then a signature header, then the main header.
// Each header is: 8-byte magic (8e ad e8 01 + reserved), 4-byte big-endian
// index-entry count, 4-byte big-endian data-store size, count*16 index entries,
// then the data store. Signature header is padded to an 8-byte boundary.

quint32 be32(const uchar *p) {
    return (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) |
           quint32(p[3]);
}

// RPM header tags we surface.
constexpr int kTagName = 1000;
constexpr int kTagVersion = 1001;
constexpr int kTagRelease = 1002;
constexpr int kTagSummary = 1004;
constexpr int kTagDescription = 1005;
constexpr int kTagGroup = 1016;
constexpr int kTagUrl = 1020;
// RPM value types that store null-terminated UTF-8 strings.
constexpr quint32 kTypeString = 6;
constexpr quint32 kTypeStringArray = 8;
constexpr quint32 kTypeI18nString = 9;

// Parse one header block starting at `start`; fill string tags into `tags` and
// report the byte offset just past the block in `endOut`. Returns false on any
// bounds/format problem.
bool parseHeader(const QByteArray &data, int start, int &endOut,
                 QMap<int, QString> &tags) {
    const uchar *p = reinterpret_cast<const uchar *>(data.constData());
    if (start < 0 || start + 16 > data.size())
        return false;
    if (!(p[start] == 0x8e && p[start + 1] == 0xad && p[start + 2] == 0xe8))
        return false;
    const quint32 nindex = be32(p + start + 8);
    const quint32 hsize = be32(p + start + 12);
    const int indexStart = start + 16;
    const qint64 dataStart = qint64(indexStart) + qint64(nindex) * 16;
    const qint64 end = dataStart + qint64(hsize);
    if (end > data.size() || nindex > 100000)
        return false;

    for (quint32 i = 0; i < nindex; ++i) {
        const uchar *e = p + indexStart + i * 16;
        const quint32 tag = be32(e);
        const quint32 type = be32(e + 4);
        const quint32 off = be32(e + 8);
        if (type != kTypeString && type != kTypeStringArray &&
            type != kTypeI18nString)
            continue;
        qint64 s = dataStart + qint64(off);
        if (s < dataStart || s >= end)
            continue;
        // The first string of an array/i18n set at `off` is the default locale.
        QByteArray str;
        while (s < data.size() && p[s] != 0) {
            str.append(char(p[s]));
            ++s;
        }
        tags.insert(static_cast<int>(tag), QString::fromUtf8(str));
    }
    endOut = static_cast<int>(end);
    return true;
}

QString readRpmInfo(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    // The lead + both headers live at the very start of the file; the main
    // header (with the description) is small. Read a generous prefix so we never
    // pull a whole multi-hundred-MB package into memory.
    const QByteArray data = f.read(8 * 1024 * 1024);
    const uchar *p = reinterpret_cast<const uchar *>(data.constData());
    if (data.size() < 96 ||
        !(p[0] == 0xed && p[1] == 0xab && p[2] == 0xee && p[3] == 0xdb))
        return QString();

    int sigEnd = 0;
    QMap<int, QString> sigTags; // discarded -- we only need where it ends
    if (!parseHeader(data, 96, sigEnd, sigTags))
        return QString();
    const int mainStart = (sigEnd + 7) & ~7; // pad signature header to 8 bytes

    int mainEnd = 0;
    QMap<int, QString> tags;
    if (!parseHeader(data, mainStart, mainEnd, tags))
        return QString();

    const QString name = tags.value(kTagName);
    const QString version = tags.value(kTagVersion);
    const QString release = tags.value(kTagRelease);
    const QString summary = tags.value(kTagSummary);
    const QString description = tags.value(kTagDescription);
    const QString group = tags.value(kTagGroup);
    const QString url = tags.value(kTagUrl);
    if (name.isEmpty() && summary.isEmpty() && description.isEmpty())
        return QString();

    QString out;
    if (!name.isEmpty()) {
        out += QStringLiteral("Name: %1").arg(name);
        if (!version.isEmpty()) {
            out += QStringLiteral("\nVersion: %1").arg(version);
            if (!release.isEmpty())
                out += QStringLiteral("-%1").arg(release);
        }
        out += QLatin1Char('\n');
    }
    if (!group.isEmpty())
        out += QStringLiteral("Group: %1\n").arg(group);
    if (!url.isEmpty())
        out += QStringLiteral("URL: %1\n").arg(url);
    if (!summary.isEmpty())
        out += QStringLiteral("Summary: %1\n").arg(summary);
    if (!description.isEmpty())
        out += QStringLiteral("\n%1").arg(description);
    return out.trimmed();
}

} // namespace

namespace PackageInfo {

QString forPackage(const QString &path) {
    const QString lower = path.toLower();
    if (lower.endsWith(QLatin1String(".deb")))
        return readDebControl(path);
    if (lower.endsWith(QLatin1String(".rpm")))
        return readRpmInfo(path);
    return QString();
}

} // namespace PackageInfo
