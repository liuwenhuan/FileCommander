#include "ScratchDirs.h"

#include <QDir>
#include <QLatin1String>
#include <QTemporaryDir>

ScratchDirs::ScratchDirs() = default;
ScratchDirs::~ScratchDirs() = default;

QString ScratchDirs::ensure(std::unique_ptr<QTemporaryDir> &slot, const char *nameFragment,
                            bool removeOnExit) {
    if (!slot) {
        slot = std::make_unique<QTemporaryDir>(
            QDir::tempPath() + QLatin1String("/FileCommander-") + QLatin1String(nameFragment) +
            QLatin1String("-XXXXXX"));
        // Set before anyone can look at the path, so a directory that must
        // survive cannot be removed by an early destruction.
        slot->setAutoRemove(removeOnExit);
    }
    return slot->isValid() ? slot->path() : QString();
}

QString ScratchDirs::preview() {
    return ensure(m_preview, "preview", true);
}

QString ScratchDirs::openWith() {
    return ensure(m_openWith, "open", false);
}

QString ScratchDirs::archive() {
    return ensure(m_archive, "archive", true);
}

void ScratchDirs::discardArchive() {
    m_archive.reset();
}
