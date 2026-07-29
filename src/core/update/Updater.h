#pragma once

#include <QObject>
#include <QString>

#include "UpdateChecker.h" // UpdateInfo

class QNetworkAccessManager;
class QNetworkReply;

// Downloads a release package, verifies its SHA-256, installs it according to
// how this build is running (AppImage self-replace, or .deb via pkexec), and
// relaunches the app. All steps report through the signals below; a hash
// mismatch aborts hard before anything is installed.
class Updater : public QObject {
    Q_OBJECT

public:
    explicit Updater(QObject *parent = nullptr);

    // True when launched from an AppImage bundle (the AppImage runtime exports
    // $APPIMAGE pointing at the .AppImage file). False for a system/.deb install.
    static bool runningAsAppImage();

    // Download -> verify SHA-256 -> install by form -> relaunch. Emits progress()
    // during the download and exactly one finished() at the end. On a successful
    // AppImage/deb update the replacement process is already spawned when
    // finished(true, ...) fires, so the caller should quit the current instance.
    void apply(const UpdateInfo &info);

signals:
    void progress(int percent);
    void finished(bool ok, const QString &message);

private:
    void onDownloadFinished(QNetworkReply *reply, const QString &downloadedFile,
                            const UpdateInfo &info);
#ifdef Q_OS_WIN
    void installWindowsPortable(const QString &downloadedFile, const UpdateInfo &info);
#endif
    void installAppImage(const QString &downloadedFile, const UpdateInfo &info);
    void installDeb(const QString &downloadedFile, const UpdateInfo &info);

    QNetworkAccessManager *m_net;
};
