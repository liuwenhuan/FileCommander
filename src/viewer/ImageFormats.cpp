#include "ImageFormats.h"

#include <QFileInfo>
#include <QSet>

namespace fc {

bool isImage(const QString &path) {
    static const QSet<QString> kImageSuffixes = {"png",  "jpg", "jpeg", "gif",
                                                 "bmp",  "svg", "webp", "ico"};
    const QString name = QFileInfo(path).fileName();
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    const QString suffix = dot > 0 ? name.mid(dot + 1).toLower() : QString();
    return kImageSuffixes.contains(suffix);
}

} // namespace fc
