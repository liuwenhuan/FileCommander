#include "DirectoryChangeMonitor.h"

DirectoryChangeMonitor::DirectoryChangeMonitor(QObject *parent) : QObject(parent) {}

DirectoryChangeMonitor::~DirectoryChangeMonitor() {
    stopWatching();
}

bool DirectoryChangeMonitor::startWatching(const QString &path) {
    stopWatching();
    m_path = path;
    if (m_path.isEmpty()) {
        m_state = State::NeedsReconciliation;
        emit stateChanged(m_state);
        emit reconciliationRequired();
        return false;
    }

    if (!startNative(m_path)) {
        m_state = State::NeedsReconciliation;
        emit stateChanged(m_state);
        emit reconciliationRequired();
        return false;
    }

    m_state = State::Watching;
    emit stateChanged(m_state);
    return true;
}

void DirectoryChangeMonitor::stopWatching() {
    stopNative();
    m_path.clear();
    if (m_state != State::Stopped) {
        m_state = State::Stopped;
        emit stateChanged(m_state);
    }
}

void DirectoryChangeMonitor::notifyChanged() {
    if (m_state == State::Watching)
        emit changesDetected();
}

void DirectoryChangeMonitor::requireReconciliation() {
    if (m_state == State::NeedsReconciliation)
        return;
    stopNative();
    m_state = State::NeedsReconciliation;
    emit stateChanged(m_state);
    emit reconciliationRequired();
}
