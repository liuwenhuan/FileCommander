#pragma once

#include <QByteArray>

// Extracts the small preview image that cameras and phones embed in a JPEG's
// EXIF APP1 segment.
//
// This exists for one case: thumbnailing a large photo on a remote share. A
// 23 MB camera JPEG has to be fetched whole to decode -- a truncated JPEG is
// worse than useless, because Qt decodes it *without reporting an error* and
// yields an image that is mostly flat grey. But the embedded preview is a
// complete, self-contained JPEG that sits within the first few tens of KB, so
// reading just the head of the file yields a real thumbnail at a few hundredths
// of the cost.
namespace ExifThumbnail {

// Returns the embedded preview JPEG found in `head` (the leading bytes of a
// JPEG file), or an empty QByteArray when there is none. `head` may be a
// partial file: parsing stops cleanly at the end of what is present, so a
// segment that runs past the buffer simply yields "not found" rather than
// reading out of bounds.
//
// The result is raw JPEG bytes ready to hand to QImage/QImageReader. Nothing is
// decoded here, so this is cheap enough to try before falling back to fetching
// the whole file.
QByteArray extract(const QByteArray &head);

} // namespace ExifThumbnail
