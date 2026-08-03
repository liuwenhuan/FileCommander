#pragma once

#include <QFile>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>

#include "UpdateChecker.h" // UpdateInfo

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

// Downloads a release package, verifies its SHA-256, installs it according to
// how this build is running (Windows portable ZIP, AppImage self-replace, or
// .deb via pkexec), and relaunches the app. All steps report through the signals
// below; a hash mismatch aborts hard before anything is installed.
//
// The download survives an unreliable link: bytes land in a persistent .part
// file next to where the finished package will go, a broken connection is
// retried with backoff, and each attempt asks the server to continue from where
// the last one stopped (HTTP Range). None of that weakens verification -- the
// assembled file is hashed exactly as before, which is also what makes resuming
// safe when a server hands back something other than the range we asked for.
class Updater : public QObject {
    Q_OBJECT

public:
    explicit Updater(QObject *parent = nullptr);

    // True when launched from an AppImage bundle (the AppImage runtime exports
    // $APPIMAGE pointing at the .AppImage file). False for a system/.deb install.
    static bool runningAsAppImage();

    // Download -> verify SHA-256 -> install by form -> relaunch. Emits progress()
    // during the download and exactly one finished() at the end. On a successful
    // update the replacement process is already spawned when finished(true, ...)
    // fires, so the caller should close the current instance.
    void apply(const UpdateInfo &info);

    // Aborts an in-flight download, including while it is waiting to retry.
    // Safe at any time; a no-op once installation has begun (the install steps
    // are each atomic or self-restoring). Emits finished(false, ...).
    void cancel();

    // How long the download may go without receiving a single byte before that
    // attempt is treated as dead. Default 60s, 0 disables. Distinct from a
    // total-time limit: a genuinely slow link must still be allowed to finish.
    void setStallTimeoutMs(int ms);

    // How many failures in a row give up. An attempt that received data resets
    // this, so a link that keeps dropping but keeps making headway still
    // converges; kMaxTotalAttempts is the backstop for one that does not.
    void setMaxConsecutiveFailures(int attempts);

    // Backoff schedule between attempts, in milliseconds. The last entry is
    // reused once the schedule runs out. Settable so tests do not sleep.
    void setRetryDelaysMs(const QVector<int> &delays);

    // Where partial and completed packages are kept. Defaults to the
    // application's cache location. A partial download in here is what makes
    // resuming possible across a restart of the whole application.
    void setDownloadDirectory(const QString &directory);
    QString downloadDirectory() const;

    // Absolute path of the completed package for `info`, and of the partial file
    // that precedes it. Public so a caller (or a test) can reason about what is
    // already on disk.
    QString packagePathFor(const UpdateInfo &info) const;
    QString partialPathFor(const UpdateInfo &info) const;

    // Never more than this many attempts for one apply(), whatever progress
    // reports. Guards the "receives one byte, then dies" case, which would
    // otherwise reset the consecutive-failure count forever.
    static constexpr int kMaxTotalAttempts = 50;

#ifdef Q_OS_WIN
    // The PowerShell installer, verbatim. Exposed so a test can run the real
    // script against a scratch directory rather than against the live install.
    static QByteArray windowsInstallScript();
#endif

signals:
    void progress(int percent);
    // A failed attempt is about to be retried: which attempt is coming, out of
    // how many consecutive failures are tolerated, and the wait before it. The
    // dialog turns this into "connection lost, retrying in 4s (2/5)".
    void retryScheduled(int attempt, int maxAttempts, int delayMs);
    void finished(bool ok, const QString &message);

protected:
    // Hands the verified package to the installer for this build's form. Called
    // only after the SHA-256 has matched. Virtual so a test can drive the whole
    // download-and-verify path without the last step replacing the test runner's
    // own executable.
    virtual void install(const QString &downloadedFile, const UpdateInfo &info);

private:
    void startAttempt();
    void onHeaders(QNetworkReply *reply);
    void onAttemptFinished(QNetworkReply *reply);
    void finishVerifiedDownload();
    // Decides between giving up and trying again, and schedules the retry.
    void handleAttemptFailure(const QString &reason, bool retryable);
    void install(const UpdateInfo &info);
#ifdef Q_OS_WIN
    void installWindowsPortable(const QString &downloadedFile, const UpdateInfo &info);
#else
    void installAppImage(const QString &downloadedFile, const UpdateInfo &info);
    void installDeb(const QString &downloadedFile, const UpdateInfo &info);
    // Second half of installDeb: runs after the pkexec child exits, so the UI
    // thread is never blocked while the user is typing into the auth prompt.
    void onDebInstallerFinished(int exitCode, const QString &debPath, const UpdateInfo &info,
                                bool triedFallback, const QString &detail);
#endif
    void fail(const QString &message);
    void succeed(const QString &message);
    void clearStallTimer();
    void closePartFile();
    void discardPartialDownload();
    void sweepStaleDownloads();
    QString storedValidator() const;
    void storeValidator(const QString &validator);

    QNetworkAccessManager *m_net;
    QPointer<QNetworkReply> m_reply;
    QTimer *m_stallTimer = nullptr;
    QTimer *m_retryTimer = nullptr;
    int m_stallTimeoutMs = 60000;
    bool m_cancelled = false;
    bool m_done = false; // guards against emitting finished() twice

    UpdateInfo m_info;
    QString m_downloadDir;
    QString m_packagePath;
    QString m_partPath;
    QFile m_partFile;
    qint64 m_resumeOffset = 0;   // bytes already on disk when this attempt began
    qint64 m_expectedTotal = -1; // full package size, once the server says
    bool m_headersHandled = false;
    bool m_bodyIsPackageData = true; // false once a non-2xx status is seen
    bool m_madeProgressThisAttempt = false;
    bool m_resumedThisAttempt = false;
    bool m_retriedAfterBadResume = false;
    int m_consecutiveFailures = 0;
    int m_totalAttempts = 0;
    int m_maxConsecutiveFailures = 5;
    QVector<int> m_retryDelaysMs{2000, 4000, 8000, 16000, 30000};
};
