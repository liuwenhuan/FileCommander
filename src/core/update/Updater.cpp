#include "Updater.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#ifndef Q_OS_WIN
#include <sys/stat.h> // chmod
#endif

namespace {

// Compute the SHA-256 of a file, streaming it so we never hold the whole
// (potentially large) package in memory. Returns an empty string on failure.
QString sha256OfFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
        return QString();
    return QString::fromLatin1(hash.result().toHex());
}

#ifdef Q_OS_WIN
// Runs after the app it is replacing has exited, so it cannot report anything
// through the UI -- everything it learns goes into update.log beside the
// staged archive, and every failure path still puts the user back into a
// running application rather than leaving them with nothing.
const char kWindowsInstallScript[] = R"PS(param(
    [int]$ProcessId,
    [string]$Archive,
    [string]$Target,
    [string]$Executable,
    [string]$Root,
    [int]$WaitSeconds = 120
)
$ErrorActionPreference = 'Stop'
$log = Join-Path $Root 'update.log'
function Write-Log($message) {
    Add-Content -LiteralPath $log -Value ('[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $message)
}
$live = Join-Path $Target $Executable
try {
    Write-Log ('waiting for pid {0}' -f $ProcessId)
    # Bounded: if the old process never exits, its files stay locked and the
    # copy below would fail anyway. Give up cleanly instead of spinning forever.
    $deadline = (Get-Date).AddSeconds($WaitSeconds)
    while (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue) {
        if ((Get-Date) -gt $deadline) {
            throw ('the previous instance (pid {0}) is still running after {1}s' -f $ProcessId, $WaitSeconds)
        }
        Start-Sleep -Milliseconds 200
    }

    $stage = Join-Path $Root 'stage'
    Expand-Archive -LiteralPath $Archive -DestinationPath $stage -Force
    # The archive may hold the payload at its root or inside one wrapper
    # directory (which is how the release ZIP is laid out). Anything else is
    # not a package we know how to install.
    $payload = $stage
    if (-not (Test-Path -LiteralPath (Join-Path $payload $Executable))) {
        $children = @(Get-ChildItem -LiteralPath $stage -Directory)
        if ($children.Count -eq 1 -and (Test-Path -LiteralPath (Join-Path $children[0].FullName $Executable))) {
            $payload = $children[0].FullName
        } else {
            throw 'the update archive does not contain the application executable'
        }
    }

    # Keep the executable being replaced, so a copy that fails half way can be
    # undone. The rest of the install is additive (new files overwrite old ones
    # of the same name), so the executable is the only thing that must not be
    # left in a torn state.
    $backup = Join-Path $Root 'previous.exe'
    if (Test-Path -LiteralPath $live) {
        Copy-Item -LiteralPath $live -Destination $backup -Force
    }
    try {
        Get-ChildItem -LiteralPath $payload | Copy-Item -Destination $Target -Recurse -Force
    } catch {
        if (Test-Path -LiteralPath $backup) {
            Copy-Item -LiteralPath $backup -Destination $live -Force
            Write-Log 'copy failed; restored the previous executable'
        }
        throw
    }
    Write-Log 'update applied'
    Start-Process -FilePath $live -WorkingDirectory $Target
    # The log has served its purpose on the success path.
    Remove-Item -LiteralPath $Root -Recurse -Force -ErrorAction SilentlyContinue
    exit 0
} catch {
    Write-Log ('FAILED: ' + $_.Exception.Message)
    # $Root is deliberately left behind: update.log is the only record of what
    # went wrong, and the app that would have shown it is gone.
    if (Test-Path -LiteralPath $live) {
        Start-Process -FilePath $live -WorkingDirectory $Target
    }
    exit 1
}
)PS";
#endif

} // namespace

Updater::Updater(QObject *parent)
    : QObject(parent), m_net(new QNetworkAccessManager(this)) {}

bool Updater::runningAsAppImage() {
    return !qEnvironmentVariableIsEmpty("APPIMAGE");
}

#ifdef Q_OS_WIN
QByteArray Updater::windowsInstallScript() {
    return QByteArray(kWindowsInstallScript);
}
#endif

void Updater::setStallTimeoutMs(int ms) {
    m_stallTimeoutMs = ms;
}

void Updater::fail(const QString &message) {
    if (m_done)
        return;
    m_done = true;
    clearStallTimer();
    emit finished(false, message);
}

void Updater::succeed(const QString &message) {
    if (m_done)
        return;
    m_done = true;
    clearStallTimer();
    emit finished(true, message);
}

void Updater::clearStallTimer() {
    if (!m_stallTimer)
        return;
    m_stallTimer->stop();
    m_stallTimer->deleteLater();
    m_stallTimer = nullptr;
}

void Updater::cancel() {
    if (m_done || m_cancelled)
        return;
    m_cancelled = true;
    if (m_reply && m_reply->isRunning())
        m_reply->abort(); // the finished handler reports the cancellation
    else
        fail(tr("Update cancelled."));
}

void Updater::apply(const UpdateInfo &info) {
    m_done = false;
    m_cancelled = false;

    if (info.url.isEmpty() || info.sha256.isEmpty()) {
        fail(tr("Update information is incomplete."));
        return;
    }

    // Download into a private temp file. Keep the package's suffix so the deb
    // installer and AppImage runtime recognise it.
    const QString suffix = QFileInfo(QUrl(info.url).path()).suffix();
    const QString tmpDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    const QString tmpTemplate =
        QDir(tmpDir).filePath(suffix.isEmpty() ? QStringLiteral("FileCommander-update-XXXXXX")
                                               : QStringLiteral("FileCommander-update-XXXXXX.") + suffix);

    auto *tmp = new QTemporaryFile(tmpTemplate, this);
    tmp->setAutoRemove(true);
    if (!tmp->open()) {
        tmp->deleteLater();
        fail(tr("Could not create a temporary file for the download."));
        return;
    }
    const QString tmpPath = tmp->fileName();

    QNetworkRequest request{QUrl(info.url)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = m_net->get(request);
    m_reply = reply;

    // A connection that goes quiet mid-transfer would otherwise hang the dialog
    // on "Downloading…" forever. Reset the clock on every byte, so a slow but
    // live link is never punished for being slow.
    if (m_stallTimeoutMs > 0) {
        m_stallTimer = new QTimer(this);
        m_stallTimer->setSingleShot(true);
        connect(m_stallTimer, &QTimer::timeout, this, [this] {
            if (m_reply && m_reply->isRunning())
                m_reply->abort();
        });
        m_stallTimer->start(m_stallTimeoutMs);
    }

    // Stream the body to disk as it arrives instead of buffering it in memory.
    connect(reply, &QNetworkReply::readyRead, this, [this, reply, tmp]() {
        if (m_stallTimer)
            m_stallTimer->start(m_stallTimeoutMs);
        tmp->write(reply->readAll());
    });
    connect(reply, &QNetworkReply::downloadProgress, this,
            [this](qint64 received, qint64 total) {
                if (total > 0)
                    emit progress(static_cast<int>(received * 100 / total));
            });
    connect(reply, &QNetworkReply::finished, this, [this, reply, tmp, tmpPath, info]() {
        tmp->flush();
        tmp->close();
        onDownloadFinished(reply, tmpPath, info);
        // Keep the file alive until installation has consumed it; the install
        // steps are synchronous up to the point where they have copied it, so
        // it is safe to drop the handle now.
        tmp->deleteLater();
    });
}

void Updater::onDownloadFinished(QNetworkReply *reply, const QString &tmpPath,
                                 const UpdateInfo &info) {
    reply->deleteLater();
    clearStallTimer();

    if (m_cancelled) {
        fail(tr("Update cancelled."));
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        if (reply->error() == QNetworkReply::OperationCanceledError)
            fail(tr("The download stopped responding and was aborted."));
        else
            fail(tr("Download failed: %1").arg(reply->errorString()));
        return;
    }
    if (tmpPath.isEmpty() || !QFile::exists(tmpPath)) {
        fail(tr("The downloaded file could not be found."));
        return;
    }

    // Verify integrity before touching anything on disk. A mismatch aborts hard.
    const QString actual = sha256OfFile(tmpPath);
    if (actual.isEmpty()) {
        fail(tr("Could not read the downloaded file for verification."));
        return;
    }
    if (actual.compare(info.sha256.trimmed(), Qt::CaseInsensitive) != 0) {
        fail(tr("Checksum mismatch — the download may be corrupt or tampered with. "
                "Update aborted."));
        return;
    }

    install(tmpPath, info);
}

void Updater::install(const QString &downloadedFile, const UpdateInfo &info) {
#ifdef Q_OS_WIN
    installWindowsPortable(downloadedFile, info);
#else
    if (runningAsAppImage())
        installAppImage(downloadedFile, info);
    else
        installDeb(downloadedFile, info);
#endif
}

#ifdef Q_OS_WIN
void Updater::installWindowsPortable(const QString &downloadedFile, const UpdateInfo &info) {
    if (!downloadedFile.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive)) {
        fail(tr("Windows portable updates must be ZIP packages."));
        return;
    }

    const QString targetDir = QCoreApplication::applicationDirPath();
    QTemporaryFile writeProbe(QDir(targetDir).filePath(QStringLiteral(".FileCommander-update-XXXXXX")));
    if (!writeProbe.open()) {
        fail(tr("The application folder is not writable. Extract the update manually."));
        return;
    }
    writeProbe.close();

    const QString root = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                             .filePath(QStringLiteral("FileCommander-update-%1")
                                           .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!QDir().mkpath(root)) {
        fail(tr("Could not create the update staging directory."));
        return;
    }
    const QString archive = QDir(root).filePath(QStringLiteral("package.zip"));
    if (!QFile::copy(downloadedFile, archive)) {
        QDir(root).removeRecursively();
        fail(tr("Could not stage the downloaded update."));
        return;
    }

    const QString scriptPath = QDir(root).filePath(QStringLiteral("apply-update.ps1"));
    QFile script(scriptPath);
    if (!script.open(QIODevice::WriteOnly)) {
        QDir(root).removeRecursively();
        fail(tr("Could not prepare the update installer."));
        return;
    }
    script.write(windowsInstallScript());
    script.close();

    const QString executable = QFileInfo(QCoreApplication::applicationFilePath()).fileName();
    const QStringList args{QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"),
                           QStringLiteral("Bypass"), QStringLiteral("-File"), scriptPath,
                           QStringLiteral("-ProcessId"), QString::number(QCoreApplication::applicationPid()),
                           QStringLiteral("-Archive"), archive, QStringLiteral("-Target"), targetDir,
                           QStringLiteral("-Executable"), executable, QStringLiteral("-Root"), root};
    if (!QProcess::startDetached(QStringLiteral("powershell.exe"), args)) {
        QDir(root).removeRecursively();
        fail(tr("Could not launch the Windows update installer."));
        return;
    }
    succeed(tr("Updated to version %1. Restarting…").arg(info.version));
}
#endif

#ifndef Q_OS_WIN
void Updater::installAppImage(const QString &downloadedFile, const UpdateInfo &info) {
    const QByteArray appImageEnv = qgetenv("APPIMAGE");
    const QString target = QString::fromLocal8Bit(appImageEnv);
    if (target.isEmpty()) {
        fail(tr("Could not determine the AppImage path to replace."));
        return;
    }

    // Write the replacement next to the target first, then atomically rename over
    // it — a half-written AppImage never becomes the live one.
    const QString stagePath = target + QStringLiteral(".new");
    QFile::remove(stagePath);
    if (!QFile::copy(downloadedFile, stagePath)) {
        fail(tr("Could not stage the new AppImage next to %1.").arg(target));
        return;
    }

    // Make the staged file executable (rwxr-xr-x) so it can relaunch.
    if (::chmod(stagePath.toLocal8Bit().constData(), 0755) != 0) {
        QFile::remove(stagePath);
        fail(tr("Could not make the new AppImage executable."));
        return;
    }

    // rename() replaces the target in place. Remove first is not needed on Linux
    // (rename is atomic and overwrites), but guard the error path.
    if (!QFile::remove(target) || !QFile::rename(stagePath, target)) {
        // Best effort: if the rename failed, try to leave the original intact.
        QFile::remove(stagePath);
        fail(tr("Could not replace the running AppImage at %1.").arg(target));
        return;
    }

    if (!QProcess::startDetached(target, QStringList())) {
        fail(tr("Updated, but could not relaunch %1 automatically. "
                "Please start it again manually.")
                 .arg(target));
        return;
    }

    succeed(tr("Updated to version %1. Restarting…").arg(info.version));
}

void Updater::installDeb(const QString &downloadedFile, const UpdateInfo &info) {
    // Give the package a stable, .deb-suffixed name so apt/dpkg accept it.
    const QString debPath = downloadedFile.endsWith(QStringLiteral(".deb"))
                                ? downloadedFile
                                : downloadedFile + QStringLiteral(".deb");
    if (debPath != downloadedFile) {
        QFile::remove(debPath);
        if (!QFile::copy(downloadedFile, debPath)) {
            fail(tr("Could not prepare the downloaded package for installation."));
            return;
        }
    }

    // Installing to system paths needs root; pkexec raises a graphical prompt.
    // Prefer `apt-get install` (pulls dependencies); fall back to `dpkg -i`.
    //
    // Asynchronous on purpose: the user is about to type a password into that
    // prompt, and a waitForFinished() here would freeze the window behind it --
    // including the progress dialog that is telling them what is happening.
    auto *apt = new QProcess(this);
    connect(apt, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, apt, debPath, info](int exitCode, QProcess::ExitStatus status) {
                const QString detail =
                    QString::fromLocal8Bit(apt->readAllStandardError()).trimmed();
                apt->deleteLater();
                onDebInstallerFinished(status == QProcess::NormalExit ? exitCode : -1, debPath,
                                       info, false, detail);
            });
    connect(apt, &QProcess::errorOccurred, this, [this, apt](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        apt->deleteLater();
        fail(tr("Could not launch the installer (pkexec not available)."));
    });
    apt->start(QStringLiteral("pkexec"),
               {QStringLiteral("apt-get"), QStringLiteral("install"), QStringLiteral("-y"),
                debPath});
}

void Updater::onDebInstallerFinished(int exitCode, const QString &debPath, const UpdateInfo &info,
                                     bool triedFallback, const QString &detail) {
    if (exitCode != 0 && !triedFallback) {
        // Retry with dpkg -i for the offline/apt-less case.
        auto *dpkg = new QProcess(this);
        connect(dpkg, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                [this, dpkg, debPath, info](int code, QProcess::ExitStatus status) {
                    const QString err =
                        QString::fromLocal8Bit(dpkg->readAllStandardError()).trimmed();
                    dpkg->deleteLater();
                    onDebInstallerFinished(status == QProcess::NormalExit ? code : -1, debPath,
                                           info, true, err);
                });
        connect(dpkg, &QProcess::errorOccurred, this, [this, dpkg](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart)
                return;
            dpkg->deleteLater();
            fail(tr("Installation failed and no fallback installer is available."));
        });
        dpkg->start(QStringLiteral("pkexec"),
                    {QStringLiteral("dpkg"), QStringLiteral("-i"), debPath});
        return;
    }

    if (exitCode != 0) {
        fail(detail.isEmpty() ? tr("Package installation failed.")
                              : tr("Package installation failed:\n%1").arg(detail));
        return;
    }

    // Relaunch the freshly installed binary. Under an installed build the current
    // executable path points at the just-replaced binary.
    const QString appPath = QCoreApplication::applicationFilePath();
    if (!QProcess::startDetached(appPath, QStringList())) {
        fail(tr("Updated to version %1, but could not restart automatically. "
                "Please start FileCommander again.")
                 .arg(info.version));
        return;
    }

    succeed(tr("Updated to version %1. Restarting…").arg(info.version));
}
#endif
