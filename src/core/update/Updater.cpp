#include "Updater.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QDateTime>
#include <QProcess>
#include <QRegularExpression>
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

void Updater::setMaxConsecutiveFailures(int attempts) {
    m_maxConsecutiveFailures = qMax(1, attempts);
}

void Updater::setRetryDelaysMs(const QVector<int> &delays) {
    if (!delays.isEmpty())
        m_retryDelaysMs = delays;
}

void Updater::setDownloadDirectory(const QString &directory) {
    m_downloadDir = directory;
}

QString Updater::downloadDirectory() const {
    if (!m_downloadDir.isEmpty())
        return m_downloadDir;
    const QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    return QDir(base.isEmpty()
                    ? QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                    : base)
        .filePath(QStringLiteral("updates"));
}

QString Updater::packagePathFor(const UpdateInfo &info) const {
    // Named for the version AND the hash: two builds carrying the same version
    // number (which happens during testing, and when a release is re-spun) must
    // never be mistaken for one another and resumed into each other's file.
    const QString suffix = QFileInfo(QUrl(info.url).path()).suffix();
    QString name = QStringLiteral("FileCommander-update-%1-%2")
                       .arg(info.version.isEmpty() ? QStringLiteral("unknown") : info.version,
                            info.sha256.left(12).toLower());
    name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")), QStringLiteral("_"));
    if (!suffix.isEmpty())
        name += QLatin1Char('.') + suffix;
    return QDir(downloadDirectory()).filePath(name);
}

QString Updater::partialPathFor(const UpdateInfo &info) const {
    return packagePathFor(info) + QStringLiteral(".part");
}

QString Updater::storedValidator() const {
    QFile file(m_partPath + QStringLiteral(".meta"));
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(file.readAll()).trimmed();
}

void Updater::storeValidator(const QString &validator) {
    const QString path = m_partPath + QStringLiteral(".meta");
    if (validator.isEmpty()) {
        QFile::remove(path);
        return;
    }
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(validator.toUtf8());
}

void Updater::closePartFile() {
    if (m_partFile.isOpen()) {
        m_partFile.flush();
        m_partFile.close();
    }
}

void Updater::discardPartialDownload() {
    closePartFile();
    QFile::remove(m_partPath);
    QFile::remove(m_partPath + QStringLiteral(".meta"));
    m_resumeOffset = 0;
}

// Partial downloads for releases nobody is installing any more would otherwise
// accumulate forever, and each one is the size of a release.
void Updater::sweepStaleDownloads() {
    QDir dir(downloadDirectory());
    if (!dir.exists())
        return;
    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-7);
    const QFileInfoList entries =
        dir.entryInfoList({QStringLiteral("FileCommander-update-*")}, QDir::Files);
    for (const QFileInfo &entry : entries) {
        const QString path = entry.absoluteFilePath();
        if (path == m_packagePath || path == m_partPath
            || path == m_partPath + QStringLiteral(".meta"))
            continue;
        if (entry.lastModified() < cutoff)
            QFile::remove(path);
    }
}

void Updater::cancel() {
    if (m_done || m_cancelled)
        return;
    m_cancelled = true;
    if (m_retryTimer && m_retryTimer->isActive()) {
        // Cancelling during the wait between attempts has to work too, or the
        // button does nothing for as long as the backoff has left to run.
        m_retryTimer->stop();
        closePartFile();
        fail(tr("Update cancelled."));
        return;
    }
    if (m_reply && m_reply->isRunning())
        m_reply->abort(); // the finished handler reports the cancellation
    else
        fail(tr("Update cancelled."));
}

void Updater::apply(const UpdateInfo &info) {
    m_done = false;
    m_cancelled = false;
    m_consecutiveFailures = 0;
    m_totalAttempts = 0;
    m_retriedAfterBadResume = false;
    m_expectedTotal = -1;

    if (info.url.isEmpty() || info.sha256.isEmpty()) {
        fail(tr("Update information is incomplete."));
        return;
    }

    m_info = info;
    if (!QDir().mkpath(downloadDirectory())) {
        fail(tr("Could not create a directory for the download."));
        return;
    }
    m_packagePath = packagePathFor(info);
    m_partPath = partialPathFor(info);
    sweepStaleDownloads();

    // A package that is already here and already correct is an update with
    // nothing left to download -- the normal case when an install failed and
    // the user pressed Retry, and the reason a completed file keeps its name.
    if (QFile::exists(m_packagePath)
        && sha256OfFile(m_packagePath).compare(info.sha256.trimmed(), Qt::CaseInsensitive) == 0) {
        emit progress(100);
        install(m_packagePath, info);
        return;
    }
    QFile::remove(m_packagePath); // stale or corrupt: it is about to be rebuilt

    startAttempt();
}

void Updater::startAttempt() {
    if (m_cancelled || m_done)
        return;
    ++m_totalAttempts;
    m_headersHandled = false;
    m_madeProgressThisAttempt = false;
    m_resumedThisAttempt = false;
    m_bodyIsPackageData = true; // until the status says otherwise

    // Whatever survived the last attempt is where this one starts: the file is
    // opened for append, and nothing already on disk is fetched twice.
    const qint64 have = QFileInfo::exists(m_partPath) ? QFileInfo(m_partPath).size() : 0;
    m_resumeOffset = qMax<qint64>(0, have);
    m_partFile.setFileName(m_partPath);
    const QIODevice::OpenMode mode = m_resumeOffset > 0
                                         ? (QIODevice::WriteOnly | QIODevice::Append)
                                         : (QIODevice::WriteOnly | QIODevice::Truncate);
    if (!m_partFile.open(mode)) {
        fail(tr("Could not open the download file for writing."));
        return;
    }

    QNetworkRequest request{QUrl(m_info.url)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    if (m_resumeOffset > 0) {
        request.setRawHeader("Range", "bytes=" + QByteArray::number(m_resumeOffset) + "-");
        // If-Range makes the server decide: continue the same entity (206), or
        // send the whole of a different one (200). Without a validator we still
        // ask for the range and let the SHA-256 catch a bad splice; with one,
        // the bad splice never happens in the first place.
        const QString validator = storedValidator();
        if (!validator.isEmpty())
            request.setRawHeader("If-Range", validator.toUtf8());
        m_resumedThisAttempt = true;
    }

    QNetworkReply *reply = m_net->get(request);
    m_reply = reply;

    if (m_stallTimeoutMs > 0) {
        clearStallTimer();
        m_stallTimer = new QTimer(this);
        m_stallTimer->setSingleShot(true);
        connect(m_stallTimer, &QTimer::timeout, this, [this] {
            if (m_reply && m_reply->isRunning())
                m_reply->abort();
        });
        m_stallTimer->start(m_stallTimeoutMs);
    }

    connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
        if (m_stallTimer)
            m_stallTimer->start(m_stallTimeoutMs);
        if (!m_headersHandled)
            onHeaders(reply);
        const QByteArray chunk = reply->readAll();
        if (chunk.isEmpty())
            return;
        // An error response has a body too -- an HTML page, a JSON error, a
        // proxy notice. It is not part of the package: appending it would
        // corrupt the file, and treating it as progress would tell the retry
        // budget the link is working when nothing is being delivered.
        if (!m_bodyIsPackageData)
            return;
        m_madeProgressThisAttempt = true;
        m_partFile.write(chunk);
    });
    connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64) {
        // Reported against the whole package, not against this attempt: after a
        // resume the request only carries the tail, and a bar that restarted at
        // zero would tell the user the opposite of what happened.
        if (m_expectedTotal <= 0)
            return;
        const qint64 done = m_resumeOffset + received;
        emit progress(static_cast<int>(qMin<qint64>(100, done * 100 / m_expectedTotal)));
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] { onAttemptFinished(reply); });
}

// Runs once per attempt, as soon as the status line and headers are known and
// before any of the body has reached the file.
void Updater::onHeaders(QNetworkReply *reply) {
    m_headersHandled = true;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    m_bodyIsPackageData = status == 200 || status == 206;

    if (m_resumeOffset > 0 && status != 206 && status < 400) {
        // The server did not honour the range -- either it cannot, or If-Range
        // told it the entity had changed. Either way what follows is a whole
        // file, so everything kept from before has to go before a byte of it
        // lands, or the two would be spliced into a package that never hashes.
        closePartFile();
        QFile::remove(m_partPath);
        m_resumeOffset = 0;
        m_resumedThisAttempt = false;
        m_partFile.setFileName(m_partPath);
        m_partFile.open(QIODevice::WriteOnly | QIODevice::Truncate);
    }

    // Remember what a later attempt will ask the server to match against.
    const QString etag = QString::fromUtf8(reply->rawHeader("ETag")).trimmed();
    const QString lastModified = QString::fromUtf8(reply->rawHeader("Last-Modified")).trimmed();
    if (!etag.isEmpty())
        storeValidator(etag);
    else if (!lastModified.isEmpty())
        storeValidator(lastModified);

    // The full size of the package, however the server chose to describe it.
    if (status == 206) {
        const QByteArray range = reply->rawHeader("Content-Range"); // bytes A-B/TOTAL
        const int slash = range.lastIndexOf('/');
        if (slash >= 0) {
            bool ok = false;
            const qint64 total = range.mid(slash + 1).trimmed().toLongLong(&ok);
            if (ok && total > 0)
                m_expectedTotal = total;
        }
    } else if (status < 400) {
        const QVariant length = reply->header(QNetworkRequest::ContentLengthHeader);
        if (length.isValid() && length.toLongLong() > 0)
            m_expectedTotal = length.toLongLong();
    }
}

namespace {

// Whether another attempt could plausibly do better. "No" is reserved for
// answers that will not change: the package is not there, or we are not allowed
// to have it. Everything transport-shaped is worth another try.
bool isRetryable(QNetworkReply *reply) {
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status >= 500 || status == 408 || status == 429)
        return true;
    if (status >= 400)
        return false; // 404 gone, 403 refused: retrying only repeats it

    switch (reply->error()) {
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::RemoteHostClosedError:
    case QNetworkReply::HostNotFoundError:
    case QNetworkReply::TimeoutError:
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
    case QNetworkReply::ProxyConnectionClosedError:
    case QNetworkReply::ProxyTimeoutError:
    case QNetworkReply::UnknownNetworkError:
    case QNetworkReply::OperationCanceledError: // our stall timer, not the user
        return true;
    default:
        return false;
    }
}

} // namespace

void Updater::onAttemptFinished(QNetworkReply *reply) {
    reply->deleteLater();
    clearStallTimer();
    if (!m_headersHandled)
        onHeaders(reply); // a body-less response still carries a status
    // Anything still buffered belongs in the file before the file is measured.
    const QByteArray tail = reply->readAll();
    if (!tail.isEmpty() && m_bodyIsPackageData) {
        m_madeProgressThisAttempt = true;
        m_partFile.write(tail);
    }
    closePartFile();

    if (m_cancelled) {
        fail(tr("Update cancelled."));
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        const bool stalled = reply->error() == QNetworkReply::OperationCanceledError;
        handleAttemptFailure(stalled ? tr("the connection stopped responding")
                                     : reply->errorString(),
                             isRetryable(reply));
        return;
    }

    // A clean finish that did not deliver the whole package is a truncation the
    // transport did not report. It is retryable, and resuming it is precisely
    // what this machinery is for.
    const qint64 have = QFileInfo(m_partPath).size();
    if (m_expectedTotal > 0 && have < m_expectedTotal) {
        handleAttemptFailure(
            tr("the connection closed after %1 of %2 bytes").arg(have).arg(m_expectedTotal), true);
        return;
    }

    finishVerifiedDownload();
}

void Updater::finishVerifiedDownload() {
    if (!QFile::exists(m_partPath)) {
        fail(tr("The downloaded file could not be found."));
        return;
    }

    const QString actual = sha256OfFile(m_partPath);
    if (actual.isEmpty()) {
        fail(tr("Could not read the downloaded file for verification."));
        return;
    }
    if (actual.compare(m_info.sha256.trimmed(), Qt::CaseInsensitive) != 0) {
        // A resumed download that fails here is far more likely to be a stale
        // partial file than a tampered package, so give it exactly one clean
        // run from zero before calling it tampering. A download that was never
        // resumed has no such excuse and stops here.
        const bool wasResumed = m_resumedThisAttempt;
        discardPartialDownload();
        if (wasResumed && !m_retriedAfterBadResume && m_totalAttempts < kMaxTotalAttempts) {
            m_retriedAfterBadResume = true;
            m_expectedTotal = -1;
            startAttempt();
            return;
        }
        fail(tr("Checksum mismatch - the download may be corrupt or tampered with. "
                "Update aborted."));
        return;
    }

    // Only a verified file is allowed to take the package name, so the
    // "already downloaded" shortcut in apply() can trust whatever it finds.
    QFile::remove(m_packagePath);
    if (!QFile::rename(m_partPath, m_packagePath)) {
        fail(tr("Could not finalise the downloaded package."));
        return;
    }
    QFile::remove(m_partPath + QStringLiteral(".meta"));
    emit progress(100);
    install(m_packagePath, m_info);
}

void Updater::handleAttemptFailure(const QString &reason, bool retryable) {
    // An attempt that moved bytes is evidence the link works at all, so it does
    // not count against the failure budget -- otherwise a connection that drops
    // every few megabytes could never finish a large package however much of it
    // got through. kMaxTotalAttempts is what stops that being unbounded.
    if (m_madeProgressThisAttempt)
        m_consecutiveFailures = 0;
    else
        ++m_consecutiveFailures;

    const bool budgetLeft = m_consecutiveFailures < m_maxConsecutiveFailures
                            && m_totalAttempts < kMaxTotalAttempts;
    if (!retryable || !budgetLeft) {
        fail(tr("Download failed: %1").arg(reason));
        return;
    }

    const int index = qBound(0, m_consecutiveFailures, m_retryDelaysMs.size() - 1);
    const int delay = m_retryDelaysMs.at(index);
    emit retryScheduled(m_consecutiveFailures + 1, m_maxConsecutiveFailures, delay);

    if (!m_retryTimer) {
        m_retryTimer = new QTimer(this);
        m_retryTimer->setSingleShot(true);
        connect(m_retryTimer, &QTimer::timeout, this, &Updater::startAttempt);
    }
    m_retryTimer->start(delay);
}

void Updater::install(const UpdateInfo &info) {
    install(m_packagePath, info);
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
