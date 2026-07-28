#include "TrashService.h"

#include <QProcess>

namespace {
class LinuxTrashService final : public TrashService {
public:
    PlatformResult moveToTrash(const QStringList &paths) override {
        QProcess process;
        process.start(QStringLiteral("gio"), QStringList{QStringLiteral("trash")} + paths);
        if (!process.waitForStarted() || !process.waitForFinished(-1))
            return PlatformResult::failure(PlatformError::NativeFailure,
                                           QStringLiteral("Could not run gio trash."));
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
            return PlatformResult::failure(
                PlatformError::NativeFailure,
                QString::fromLocal8Bit(process.readAllStandardError()).trimmed(),
                process.exitCode());
        return PlatformResult::success();
    }
};
}

std::unique_ptr<TrashService> createTrashService() {
    return std::make_unique<LinuxTrashService>();
}
