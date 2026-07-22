#pragma once

#include <QByteArray>
#include <QString>

// Dependency-free reader for the metadata embedded in audio files. The heavy
// lifting is an ID3v2 parser (versions 2.2 / 2.3 / 2.4) used by MP3 (and some
// AIFF/WAV) files: it extracts the common text frames, the embedded cover
// picture (APIC/PIC) and the unsynchronised lyrics (USLT/ULT). When no ID3v2
// tag is present the trailing 128-byte ID3v1 tag is used as a fallback.
//
// No external dependency: the whole format is decoded by hand from the raw
// bytes, so this stays inside the `core` library with only Qt for strings.
struct AudioTags {
    QString title;
    QString artist;
    QString album;
    QString albumArtist;
    QString year;    // 4-digit year (from TYER/TDRC or ID3v1)
    QString genre;   // resolved to a name where a numeric reference is used
    QString track;   // e.g. "3" or "3/12"
    QString composer;
    QString comment;
    QString lyrics;  // unsynchronised lyrics (USLT), if any

    QByteArray coverData; // raw bytes of the embedded picture (JPEG/PNG/…)
    QString coverMime;    // MIME type of coverData ("image/jpeg", …)

    bool hasCover() const { return !coverData.isEmpty(); }
    // True when at least one human-readable field was found, so callers can tell
    // "parsed but empty" from "nothing here".
    bool hasAnyText() const {
        return !title.isEmpty() || !artist.isEmpty() || !album.isEmpty() ||
               !year.isEmpty() || !genre.isEmpty() || !track.isEmpty() ||
               !albumArtist.isEmpty() || !composer.isEmpty() || !comment.isEmpty();
    }
};

class Id3Reader {
public:
    // Reads whatever metadata can be found in the file at `path`. Always returns
    // a value; missing fields stay empty. Reads only the tag regions (the
    // ID3v2 header area at the front and the 128-byte ID3v1 trailer), never the
    // whole audio stream.
    static AudioTags read(const QString &path);

    // Maps an ID3v1 numeric genre index to its canonical name, or an empty
    // string when out of range. Exposed for reuse/testing.
    static QString genreName(int index);
};
