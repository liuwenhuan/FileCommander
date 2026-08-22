#include "TrashService.h"

#include <QProcess>
#include <QSet>

namespace {
QStringList trashUris() {
    QProcess process;
    process.start(QStringLiteral("gio"),
                  {QStringLiteral("list"), QStringLiteral("--hidden"),
                   QStringLiteral("--print-uris"), QStringLiteral("trash:///")});
    if (!process.waitForStarted() || !process.waitForFinished(-1) ||
        process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return {};
    return QString::fromUtf8(process.readAllStandardOutput())
        .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

PlatformResult runGioTrash(const QStringList &arguments) {
    QProcess process;
    process.start(QStringLiteral("gio"), QStringList{QStringLiteral("trash")} + arguments);
    if (!process.waitForStarted() || !process.waitForFinished(-1))
        return PlatformResult::failure(PlatformError::NativeFailure,
                                       QStringLiteral("Could not run gio trash."));
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return PlatformResult::failure(
            PlatformError::NativeFailure,
            QString::fromLocal8Bit(process.readAllStandardError()).trimmed(), process.exitCode());
    return PlatformResult::success();
}

class LinuxTrashService final : public TrashService {
public:
    PlatformResult moveToTrash(const QStringList &paths) override {
        const QStringList beforeEntries = trashUris();
        const QSet<QString> before(beforeEntries.cbegin(), beforeEntries.cend());
        const PlatformResult result = runGioTrash(paths);
        if (!result.ok)
            return result;

        const QStringList after = trashUris();
        QStringList added;
        for (const QString &uri : after) {
            if (!before.contains(uri))
                added << uri;
        }
        // FileOperations moves one item at a time. Refuse an ambiguous snapshot rather
        // than risk restoring a file another program sent to the trash concurrently.
        return PlatformResult::success(added.size() == 1 ? added : QStringList{});
    }

    PlatformResult restoreFromTrash(const QStringList &entries) override {
        return runGioTrash(QStringList{QStringLiteral("--restore")} + entries);
    }
};
}

std::unique_ptr<TrashService> createTrashService() {
    return std::make_unique<LinuxTrashService>();
}
