#pragma once

#include <QByteArray>
#include <QIODevice>

namespace fc {

inline bool writeUploadChunk(QIODevice &device, const QByteArray &data) {
    qint64 written = 0;
    while (written < data.size()) {
        const qint64 n = device.write(data.constData() + written, data.size() - written);
        if (n <= 0)
            return false;
        written += n;
    }
    return true;
}

} // namespace fc
