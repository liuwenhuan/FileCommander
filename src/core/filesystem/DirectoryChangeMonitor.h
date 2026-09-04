#pragma once

#include <QObject>
#include <QString>

// Watches one directory for changes made outside FileCommander. Platform
// backends report only that the listing may be stale; FileSystemModel remains
// the single owner of enumeration, sorting, filtering, and provider metadata.
class DirectoryChangeMonitor final : public QObject {
    Q_OBJECT

public:
    enum class State {
        Stopped,
        Watching,
        NeedsReconciliation,
    };

    explicit DirectoryChangeMonitor(QObject *parent = nullptr);
    ~DirectoryChangeMonitor() override;

    // Starts a nonrecursive watch for `path`. Returns true only when the native
    // watch is active. A false result is deliberately usable: callers should
    // keep showing the listing and refresh it on activation instead.
    bool startWatching(const QString &path);
    void stopWatching();

    State state() const { return m_state; }
    bool isWatching() const { return m_state == State::Watching; }
    bool needsReconciliation() const { return m_state == State::NeedsReconciliation; }
    QString path() const { return m_path; }

signals:
    // One or more native events occurred. The receiver should debounce this
    // signal and re-list the directory instead of changing rows in place.
    void changesDetected();
    // The native stream is incomplete or unavailable (overflow, invalidated
    // handle, inaccessible path, or unsupported platform).
    void reconciliationRequired();
    void stateChanged(DirectoryChangeMonitor::State state);

private:
    friend class DirectoryChangeMonitorTestAccess;

    bool startNative(const QString &path);
    void stopNative();
    void notifyChanged();
    void requireReconciliation();

    State m_state = State::Stopped;
    QString m_path;
    void *m_nativeState = nullptr;
};

Q_DECLARE_METATYPE(DirectoryChangeMonitor::State)
