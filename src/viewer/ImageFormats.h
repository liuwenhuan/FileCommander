#pragma once

#include <QString>

namespace fc {

// Whether a path names a picture the preview pane and the F3 window can draw.
// Extension-based on purpose: every caller asks this while deciding what to do
// with an entry it has not opened yet (which preview branch to take, which
// siblings to offer next/previous over), and on a network tab reading the file
// to find out would cost a round trip per entry.
bool isImage(const QString &path);

} // namespace fc
