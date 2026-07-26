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
#include <QUrl>

#include <sys/stat.h> // chmod

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

} // namespace

Updater::Updater(QObject *parent)
    : QObject(parent), m_net(new QNetworkAccessManager(this)) {}

bool Updater::runningAsAppImage() {
    return !qEnvironmentVariableIsEmpty("APPIMAGE");
}

void Updater::apply(const UpdateInfo &info) {
    if (info.url.isEmpty() || info.sha256.isEmpty()) {
        emit finished(false, tr("Update information is incomplete."));
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
        emit finished(false, tr("Could not create a temporary file for the download."));
        return;
    }
    const QString tmpPath = tmp->fileName();

    QNetworkRequest request{QUrl(info.url)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = m_net->get(request);

    // Stream the body to disk as it arrives instead of buffering it in memory.
    connect(reply, &QNetworkReply::readyRead, this, [reply, tmp]() { tmp->write(reply->readAll()); });
    connect(reply, &QNetworkReply::downloadProgress, this,
            [this](qint64 received, qint64 total) {
                if (total > 0)
                    emit progress(static_cast<int>(received * 100 / total));
            });
    connect(reply, &QNetworkReply::finished, this, [this, reply, tmp, tmpPath, info]() {
        tmp->flush();
        tmp->close();
        onDownloadFinished(reply, tmpPath, info);
        // Keep the file alive until installation has consumed it; onDownloadFinished
        // installs synchronously, so it is safe to drop the handle now.
        tmp->deleteLater();
    });
}

void Updater::onDownloadFinished(QNetworkReply *reply, const QString &tmpPath,
                                 const UpdateInfo &info) {
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        emit finished(false, tr("Download failed: %1").arg(reply->errorString()));
        return;
    }
    if (tmpPath.isEmpty() || !QFile::exists(tmpPath)) {
        emit finished(false, tr("The downloaded file could not be found."));
        return;
    }

    // Verify integrity before touching anything on disk. A mismatch aborts hard.
    const QString actual = sha256OfFile(tmpPath);
    if (actual.isEmpty()) {
        emit finished(false, tr("Could not read the downloaded file for verification."));
        return;
    }
    if (actual.compare(info.sha256.trimmed(), Qt::CaseInsensitive) != 0) {
        emit finished(false,
                      tr("Checksum mismatch — the download may be corrupt or tampered with. "
                         "Update aborted."));
        return;
    }

    if (runningAsAppImage())
        installAppImage(tmpPath, info);
    else
        installDeb(tmpPath, info);
}

void Updater::installAppImage(const QString &downloadedFile, const UpdateInfo &info) {
    const QByteArray appImageEnv = qgetenv("APPIMAGE");
    const QString target = QString::fromLocal8Bit(appImageEnv);
    if (target.isEmpty()) {
        emit finished(false, tr("Could not determine the AppImage path to replace."));
        return;
    }

    // Write the replacement next to the target first, then atomically rename over
    // it — a half-written AppImage never becomes the live one.
    const QString stagePath = target + QStringLiteral(".new");
    QFile::remove(stagePath);
    if (!QFile::copy(downloadedFile, stagePath)) {
        emit finished(false, tr("Could not stage the new AppImage next to %1.").arg(target));
        return;
    }

    // Make the staged file executable (rwxr-xr-x) so it can relaunch.
    if (::chmod(stagePath.toLocal8Bit().constData(), 0755) != 0) {
        QFile::remove(stagePath);
        emit finished(false, tr("Could not make the new AppImage executable."));
        return;
    }

    // rename() replaces the target in place. Remove first is not needed on Linux
    // (rename is atomic and overwrites), but guard the error path.
    if (!QFile::remove(target) || !QFile::rename(stagePath, target)) {
        // Best effort: if the rename failed, try to leave the original intact.
        QFile::remove(stagePath);
        emit finished(false, tr("Could not replace the running AppImage at %1.").arg(target));
        return;
    }

    if (!QProcess::startDetached(target, QStringList())) {
        emit finished(false, tr("Updated, but could not relaunch %1 automatically. "
                                "Please start it again manually.")
                                 .arg(target));
        return;
    }

    emit finished(true, tr("Updated to version %1. Restarting…").arg(info.version));
}

void Updater::installDeb(const QString &downloadedFile, const UpdateInfo &info) {
    // Give the package a stable, .deb-suffixed name so apt/dpkg accept it.
    const QString debPath = downloadedFile.endsWith(QStringLiteral(".deb"))
                                ? downloadedFile
                                : downloadedFile + QStringLiteral(".deb");
    if (debPath != downloadedFile) {
        QFile::remove(debPath);
        if (!QFile::copy(downloadedFile, debPath)) {
            emit finished(false, tr("Could not prepare the downloaded package for installation."));
            return;
        }
    }

    // Installing to system paths needs root; pkexec raises a graphical prompt.
    // Prefer `apt-get install` (pulls dependencies); fall back to `dpkg -i`.
    QProcess installer;
    installer.start(QStringLiteral("pkexec"),
                    {QStringLiteral("apt-get"), QStringLiteral("install"), QStringLiteral("-y"),
                     debPath});
    if (!installer.waitForStarted()) {
        emit finished(false, tr("Could not launch the installer (pkexec not available)."));
        return;
    }
    installer.waitForFinished(-1);

    if (installer.exitStatus() != QProcess::NormalExit || installer.exitCode() != 0) {
        // Retry with dpkg -i for the offline/apt-less case.
        QProcess dpkg;
        dpkg.start(QStringLiteral("pkexec"),
                   {QStringLiteral("dpkg"), QStringLiteral("-i"), debPath});
        if (!dpkg.waitForStarted()) {
            emit finished(false, tr("Installation failed and no fallback installer is available."));
            return;
        }
        dpkg.waitForFinished(-1);
        if (dpkg.exitStatus() != QProcess::NormalExit || dpkg.exitCode() != 0) {
            const QString detail = QString::fromLocal8Bit(dpkg.readAllStandardError()).trimmed();
            emit finished(false,
                          detail.isEmpty()
                              ? tr("Package installation failed.")
                              : tr("Package installation failed:\n%1").arg(detail));
            return;
        }
    }

    // Relaunch the freshly installed binary. Under an installed build the current
    // executable path points at the just-replaced binary.
    const QString appPath = QCoreApplication::applicationFilePath();
    if (!QProcess::startDetached(appPath, QStringList())) {
        emit finished(false, tr("Updated to version %1, but could not restart automatically. "
                                "Please start FileCommander again.")
                                 .arg(info.version));
        return;
    }

    emit finished(true, tr("Updated to version %1. Restarting…").arg(info.version));
}
