#include "Id3Reader.h"

#include <QFile>
#include <QStringList>
#include <QTextCodec>

namespace {

// The classic ID3v1 genre table (Winamp's extensions included). Indexed by the
// numeric genre byte; also referenced by ID3v2 TCON when it stores "(NN)".
const char *const kGenres[] = {
    "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge",
    "Hip-Hop", "Jazz", "Metal", "New Age", "Oldies", "Other", "Pop", "R&B",
    "Rap", "Reggae", "Rock", "Techno", "Industrial", "Alternative", "Ska",
    "Death Metal", "Pranks", "Soundtrack", "Euro-Techno", "Ambient",
    "Trip-Hop", "Vocal", "Jazz+Funk", "Fusion", "Trance", "Classical",
    "Instrumental", "Acid", "House", "Game", "Sound Clip", "Gospel", "Noise",
    "Alternative Rock", "Bass", "Soul", "Punk", "Space", "Meditative",
    "Instrumental Pop", "Instrumental Rock", "Ethnic", "Gothic", "Darkwave",
    "Techno-Industrial", "Electronic", "Pop-Folk", "Eurodance", "Dream",
    "Southern Rock", "Comedy", "Cult", "Gangsta", "Top 40", "Christian Rap",
    "Pop/Funk", "Jungle", "Native US", "Cabaret", "New Wave", "Psychadelic",
    "Rave", "Showtunes", "Trailer", "Lo-Fi", "Tribal", "Acid Punk",
    "Acid Jazz", "Polka", "Retro", "Musical", "Rock & Roll", "Hard Rock",
    "Folk", "Folk-Rock", "National Folk", "Swing", "Fast Fusion", "Bebob",
    "Latin", "Revival", "Celtic", "Bluegrass", "Avantgarde", "Gothic Rock",
    "Progressive Rock", "Psychedelic Rock", "Symphonic Rock", "Slow Rock",
    "Big Band", "Chorus", "Easy Listening", "Acoustic", "Humour", "Speech",
    "Chanson", "Opera", "Chamber Music", "Sonata", "Symphony", "Booty Bass",
    "Primus", "Porn Groove", "Satire", "Slow Jam", "Club", "Tango", "Samba",
    "Folklore", "Ballad", "Power Ballad", "Rhythmic Soul", "Freestyle",
    "Duet", "Punk Rock", "Drum Solo", "A capella", "Euro-House", "Dance Hall"};
constexpr int kGenreCount = static_cast<int>(sizeof(kGenres) / sizeof(kGenres[0]));

// A syncsafe 28-bit integer: seven bits per byte, high bit of each byte zero.
// Used for the ID3v2 tag size (all versions) and v2.4 frame sizes.
quint32 syncsafe(const uchar *p) {
    return (quint32(p[0] & 0x7f) << 21) | (quint32(p[1] & 0x7f) << 14) |
           (quint32(p[2] & 0x7f) << 7) | quint32(p[3] & 0x7f);
}

// A plain big-endian 32-bit integer: v2.3 frame sizes.
quint32 be32(const uchar *p) {
    return (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) |
           quint32(p[3]);
}

// A plain big-endian 24-bit integer: v2.2 frame sizes.
quint32 be24(const uchar *p) {
    return (quint32(p[0]) << 16) | (quint32(p[1]) << 8) | quint32(p[2]);
}

// Reverses ID3's unsynchronisation: every 0xFF 0x00 pair collapses to 0xFF.
QByteArray deUnsync(const QByteArray &in) {
    QByteArray out;
    out.reserve(in.size());
    for (int i = 0; i < in.size(); ++i) {
        const uchar c = static_cast<uchar>(in.at(i));
        out.append(static_cast<char>(c));
        if (c == 0xff && i + 1 < in.size() && static_cast<uchar>(in.at(i + 1)) == 0x00)
            ++i; // drop the inserted 0x00
    }
    return out;
}

// Decodes an ID3v2 text field per its leading-byte encoding id:
//   0 = ISO-8859-1, 1 = UTF-16 (BOM), 2 = UTF-16BE, 3 = UTF-8.
QString decodeText(int enc, const QByteArray &bytes) {
    switch (enc) {
    case 0:
        return QString::fromLatin1(bytes);
    case 1: {
        if (QTextCodec *c = QTextCodec::codecForName("UTF-16"))
            return c->toUnicode(bytes);
        return QString::fromUtf8(bytes);
    }
    case 2: {
        if (QTextCodec *c = QTextCodec::codecForName("UTF-16BE"))
            return c->toUnicode(bytes);
        return QString::fromUtf8(bytes);
    }
    case 3:
    default:
        return QString::fromUtf8(bytes);
    }
}

// Length in bytes of the string terminator for an encoding (2 for the UTF-16
// variants, 1 otherwise).
int terminatorSize(int enc) { return (enc == 1 || enc == 2) ? 2 : 1; }

// Finds the offset of the encoding-appropriate null terminator in `data`
// starting at `from`, or -1. For UTF-16 the terminator is an aligned 0x00 0x00.
int findTerminator(const QByteArray &data, int from, int enc) {
    if (terminatorSize(enc) == 2) {
        for (int i = from; i + 1 < data.size(); i += 2)
            if (data.at(i) == 0 && data.at(i + 1) == 0)
                return i;
        return -1;
    }
    for (int i = from; i < data.size(); ++i)
        if (data.at(i) == 0)
            return i;
    return -1;
}

// Strips a leading "(NN)" ID3v1 genre reference, resolving it to a name; if a
// trailing free-text refinement follows it is preferred. Handles the plain
// numeric case ("17") too.
QString resolveGenre(const QString &raw) {
    QString s = raw.trimmed();
    if (s.isEmpty())
        return s;
    if (s.startsWith('(')) {
        const int close = s.indexOf(')');
        if (close > 1) {
            bool ok = false;
            const int idx = s.mid(1, close - 1).toInt(&ok);
            const QString rest = s.mid(close + 1).trimmed();
            if (!rest.isEmpty())
                return rest;
            if (ok)
                return Id3Reader::genreName(idx);
        }
    }
    bool ok = false;
    const int idx = s.toInt(&ok);
    if (ok) {
        const QString name = Id3Reader::genreName(idx);
        if (!name.isEmpty())
            return name;
    }
    return s;
}

// Assigns a decoded text frame to the matching AudioTags field. `id` is the
// 3- or 4-char frame id; comparisons cover both the v2.2 and v2.3/2.4 spellings.
void applyTextFrame(AudioTags &tags, const QByteArray &id, const QString &value) {
    const QString v = value.trimmed();
    if (v.isEmpty())
        return;
    if (id == "TIT2" || id == "TT2")
        tags.title = v;
    else if (id == "TPE1" || id == "TP1")
        tags.artist = v;
    else if (id == "TALB" || id == "TAL")
        tags.album = v;
    else if (id == "TPE2" || id == "TP2")
        tags.albumArtist = v;
    else if (id == "TYER" || id == "TYE" || id == "TDRC" || id == "TDRL")
        tags.year = v.left(4);
    else if (id == "TCON" || id == "TCO")
        tags.genre = resolveGenre(v);
    else if (id == "TRCK" || id == "TRK")
        tags.track = v;
    else if (id == "TCOM" || id == "TCM")
        tags.composer = v;
}

// Parses an APIC (v2.3/2.4) or PIC (v2.2) picture frame into cover bytes/mime.
void parsePicture(AudioTags &tags, const QByteArray &body, bool v22) {
    if (body.isEmpty())
        return;
    const int enc = static_cast<uchar>(body.at(0));
    int pos = 1;
    QString mime;
    if (v22) {
        // v2.2 PIC: 3-char image format ("JPG", "PNG", …) rather than a MIME.
        if (body.size() < 4)
            return;
        const QString fmt = QString::fromLatin1(body.mid(1, 3)).toLower();
        mime = QStringLiteral("image/") + (fmt == QLatin1String("jpg") ? "jpeg" : fmt);
        pos = 4;
    } else {
        const int term = findTerminator(body, pos, 0); // MIME is always Latin1
        if (term < 0)
            return;
        mime = QString::fromLatin1(body.mid(pos, term - pos));
        pos = term + 1;
    }
    if (pos >= body.size())
        return;
    ++pos; // picture type byte
    // Description, terminated per the frame's text encoding.
    const int dterm = findTerminator(body, pos, enc);
    if (dterm < 0)
        return;
    pos = dterm + terminatorSize(enc);
    if (pos >= body.size())
        return;
    tags.coverData = body.mid(pos);
    tags.coverMime = mime.isEmpty() ? QStringLiteral("image/jpeg") : mime;
}

// Parses a USLT (v2.3/2.4) or ULT (v2.2) unsynchronised-lyrics frame.
void parseLyrics(AudioTags &tags, const QByteArray &body) {
    if (body.size() < 4 || !tags.lyrics.isEmpty())
        return;
    const int enc = static_cast<uchar>(body.at(0));
    int pos = 4; // encoding byte + 3-byte language code
    const int dterm = findTerminator(body, pos, enc); // content descriptor
    if (dterm < 0)
        return;
    pos = dterm + terminatorSize(enc);
    if (pos > body.size())
        return;
    tags.lyrics = decodeText(enc, body.mid(pos)).trimmed();
}

// Reads the 128-byte ID3v1 trailer as a fallback when no ID3v2 frame supplied a
// given field. Only fills fields left empty by the ID3v2 pass.
void readId3v1(const QString &path, AudioTags &tags) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return;
    if (f.size() < 128)
        return;
    if (!f.seek(f.size() - 128))
        return;
    const QByteArray tag = f.read(128);
    if (tag.size() != 128 || !tag.startsWith("TAG"))
        return;
    auto field = [&](int off, int len) {
        return QString::fromLatin1(tag.mid(off, len)).trimmed();
    };
    if (tags.title.isEmpty())
        tags.title = field(3, 30);
    if (tags.artist.isEmpty())
        tags.artist = field(33, 30);
    if (tags.album.isEmpty())
        tags.album = field(63, 30);
    if (tags.year.isEmpty())
        tags.year = field(93, 4);
    if (tags.comment.isEmpty())
        tags.comment = field(97, 30);
    // A zero byte at offset 125 marks a track number in byte 126 (ID3v1.1).
    if (tags.track.isEmpty() && tag.at(125) == 0 && static_cast<uchar>(tag.at(126)) != 0)
        tags.track = QString::number(static_cast<uchar>(tag.at(126)));
    if (tags.genre.isEmpty()) {
        const int g = static_cast<uchar>(tag.at(127));
        tags.genre = Id3Reader::genreName(g);
    }
}

} // namespace

QString Id3Reader::genreName(int index) {
    if (index < 0 || index >= kGenreCount)
        return QString();
    return QString::fromLatin1(kGenres[index]);
}

AudioTags Id3Reader::read(const QString &path) {
    AudioTags tags;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return tags;

    const QByteArray header = file.read(10);
    bool haveV2 = false;
    if (header.size() == 10 && header.startsWith("ID3")) {
        const uchar major = static_cast<uchar>(header.at(3));
        const uchar flags = static_cast<uchar>(header.at(5));
        const quint32 tagSize = syncsafe(reinterpret_cast<const uchar *>(header.constData()) + 6);
        const bool globalUnsync = flags & 0x80;
        const bool extHeader = flags & 0x40;

        // Bound the read so a corrupt size can't allocate wildly (covers can be
        // a few MB, so 64 MiB is a generous ceiling).
        const quint32 cap = 64u * 1024 * 1024;
        QByteArray body = file.read(qMin(tagSize, cap));
        if (globalUnsync)
            body = deUnsync(body);

        int pos = 0;
        // Skip an extended header when present (its own size prefix leads it).
        if (extHeader && body.size() >= 4) {
            const quint32 extSize =
                (major >= 4) ? syncsafe(reinterpret_cast<const uchar *>(body.constData()))
                             : be32(reinterpret_cast<const uchar *>(body.constData())) + 4;
            pos += qMin<int>(extSize, body.size());
        }

        const int idLen = (major <= 2) ? 3 : 4;
        const int hdrLen = (major <= 2) ? 6 : 10;

        while (pos + hdrLen <= body.size()) {
            const QByteArray id = body.mid(pos, idLen);
            if (id.at(0) == 0) // padding
                break;

            quint32 size = 0;
            int frameFlags2 = 0;
            const uchar *sp = reinterpret_cast<const uchar *>(body.constData()) + pos + idLen;
            if (major <= 2) {
                size = be24(sp);
            } else if (major == 3) {
                size = be32(sp);
                frameFlags2 = static_cast<uchar>(body.at(pos + 9));
            } else { // major >= 4: syncsafe frame sizes
                size = syncsafe(sp);
                frameFlags2 = static_cast<uchar>(body.at(pos + 9));
            }

            const int dataStart = pos + hdrLen;
            if (size == 0 || dataStart + static_cast<int>(size) > body.size())
                break;

            QByteArray frame = body.mid(dataStart, size);
            // Per-frame unsynchronisation (v2.4 format flag 0x02).
            if (major >= 4 && (frameFlags2 & 0x02))
                frame = deUnsync(frame);
            // A data-length indicator (v2.4 flag 0x01) prefixes the payload with
            // a 4-byte size; skip it so it isn't mistaken for content.
            if (major >= 4 && (frameFlags2 & 0x01) && frame.size() >= 4)
                frame = frame.mid(4);

            const char first = id.at(0);
            if ((id == "TXXX" || id == "TXX") && frame.size() > 1) {
                // User-defined text: encoding + description (null-terminated) +
                // value. Some taggers (e.g. ffmpeg) stash lyrics here rather than
                // in USLT, so pick those up when no USLT frame supplied them.
                const int enc = static_cast<uchar>(frame.at(0));
                const int dterm = findTerminator(frame, 1, enc);
                if (dterm >= 0) {
                    const QString desc = decodeText(enc, frame.mid(1, dterm - 1));
                    const int vpos = dterm + terminatorSize(enc);
                    const QString value = decodeText(enc, frame.mid(vpos)).trimmed();
                    if (tags.lyrics.isEmpty() && desc.contains("lyric", Qt::CaseInsensitive))
                        tags.lyrics = value;
                }
            } else if (first == 'T' && !frame.isEmpty()) {
                const int enc = static_cast<uchar>(frame.at(0));
                applyTextFrame(tags, id, decodeText(enc, frame.mid(1)));
            } else if (id == "APIC" || id == "PIC") {
                parsePicture(tags, frame, major <= 2);
            } else if (id == "USLT" || id == "ULT") {
                parseLyrics(tags, frame);
            } else if ((id == "COMM" || id == "COM") && tags.comment.isEmpty() &&
                       frame.size() > 4) {
                const int enc = static_cast<uchar>(frame.at(0));
                int p = 4; // encoding + 3-byte language
                const int dterm = findTerminator(frame, p, enc);
                if (dterm >= 0) {
                    p = dterm + terminatorSize(enc);
                    tags.comment = decodeText(enc, frame.mid(p)).trimmed();
                }
            }
            pos = dataStart + static_cast<int>(size);
        }
        haveV2 = tags.hasAnyText() || tags.hasCover() || !tags.lyrics.isEmpty();
    }
    file.close();

    // ID3v1 fills anything the v2 tag left blank (or the whole thing when there
    // was no v2 tag at all).
    Q_UNUSED(haveV2);
    readId3v1(path, tags);
    return tags;
}
