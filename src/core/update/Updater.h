#pragma once

#include <QObject>
#include <QPointer>
#include <QString>

#include "UpdateChecker.h" // UpdateInfo

class QNetworkAccessManager;
class QNetworkReply;
class QTemporaryFile;
class QTimer;

// Downloads a release package, verifies its SHA-256, installs it according to
// how this build is running (Windows portable ZIP, AppImage self-replace, or
// .deb via pkexec), and relaunches the app. All steps report through the signals
// below; a hash mismatch aborts hard before anything is installed.
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

    // Aborts an in-flight download. Safe at any time; a no-op once installation
    // has begun (there is no half-installed state to return to -- the install
    // steps are each atomic or self-restoring). Emits finished(false, ...).
    void cancel();

    // How long the download may go without receiving a single byte before it is
    // treated as dead. Default 60s, 0 disables. Distinct from a total-time
    // limit: a genuinely slow link must still be allowed to finish.
    void setStallTimeoutMs(int ms);

#ifdef Q_OS_WIN
    // The PowerShell installer, verbatim. Exposed so a test can run the real
    // script against a scratch directory rather than against the live install.
    static QByteArray windowsInstallScript();
#endif

signals:
    void progress(int percent);
    void finished(bool ok, const QString &message);

protected:
    // Hands the verified package to the installer for this build's form. Called
    // only after the SHA-256 has matched. Virtual so a test can drive the whole
    // download-and-verify path without the last step replacing the test runner's
    // own executable.
    virtual void install(const QString &downloadedFile, const UpdateInfo &info);

private:
    void onDownloadFinished(QNetworkReply *reply, const QString &downloadedFile,
                            const UpdateInfo &info);
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

    QNetworkAccessManager *m_net;
    QPointer<QNetworkReply> m_reply;
    QTimer *m_stallTimer = nullptr;
    int m_stallTimeoutMs = 60000;
    bool m_cancelled = false;
    bool m_done = false; // guards against emitting finished() twice
};
