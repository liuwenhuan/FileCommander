#include "RuntimeCounters.h"

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QThread>

#include <atomic>

namespace {

struct Counters {
    std::atomic<int> networkSessions{0};
    std::atomic<int> networkThreads{0};
    std::atomic<int> activeHeartbeats{0};
    std::atomic<int> transferWorkers{0};
    std::atomic<int> curlTransfers{0};
};

Counters &counters() {
    static Counters instance;
    return instance;
}

std::atomic<int> &counter(fc::RuntimeCounter value) {
    Counters &all = counters();
    switch (value) {
    case fc::RuntimeCounter::NetworkSession:
        return all.networkSessions;
    case fc::RuntimeCounter::NetworkThread:
        return all.networkThreads;
    case fc::RuntimeCounter::ActiveHeartbeat:
        return all.activeHeartbeats;
    case fc::RuntimeCounter::TransferWorker:
        return all.transferWorkers;
    case fc::RuntimeCounter::CurlTransfer:
        return all.curlTransfers;
    }
    return all.networkSessions;
}

bool isEmpty(const fc::RuntimeSnapshot &snapshot) {
    return snapshot.networkSessions == 0 && snapshot.networkThreads == 0 &&
           snapshot.activeHeartbeats == 0 && snapshot.transferWorkers == 0 &&
           snapshot.curlTransfers == 0;
}

} // namespace

namespace fc {

RuntimeSnapshot runtimeSnapshot() {
    Counters &all = counters();
    return {all.networkSessions.load(std::memory_order_relaxed),
            all.networkThreads.load(std::memory_order_relaxed),
            all.activeHeartbeats.load(std::memory_order_relaxed),
            all.transferWorkers.load(std::memory_order_relaxed),
            all.curlTransfers.load(std::memory_order_relaxed)};
}

bool reportRuntimeSnapshotIfEnabled() {
    if (qEnvironmentVariableIntValue("FILECOMMANDER_DIAGNOSTICS") != 1)
        return false;
    static std::atomic_flag reported = ATOMIC_FLAG_INIT;
    if (reported.test_and_set(std::memory_order_relaxed))
        return false;

    const RuntimeSnapshot snapshot = runtimeSnapshot();
    qInfo().nospace() << "FileCommander RuntimeSnapshot"
                      << " networkSessions=" << snapshot.networkSessions
                      << " networkThreads=" << snapshot.networkThreads
                      << " activeHeartbeats=" << snapshot.activeHeartbeats
                      << " transferWorkers=" << snapshot.transferWorkers
                      << " curlTransfers=" << snapshot.curlTransfers;
    return true;
}

bool reportFinalRuntimeSnapshotIfEnabled(int timeoutMs) {
    if (qEnvironmentVariableIntValue("FILECOMMANDER_DIAGNOSTICS") != 1)
        return false;

    const int boundedTimeoutMs = qMax(0, timeoutMs);
    QElapsedTimer elapsed;
    elapsed.start();
    while (!isEmpty(runtimeSnapshot()) && elapsed.elapsed() < boundedTimeoutMs) {
        if (QCoreApplication::instance()) {
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            const int remainingMs =
                qMax(0, boundedTimeoutMs - static_cast<int>(elapsed.elapsed()));
            QCoreApplication::processEvents(QEventLoop::AllEvents, qMin(10, remainingMs));
        }
        if (!isEmpty(runtimeSnapshot()) && elapsed.elapsed() < boundedTimeoutMs)
            QThread::msleep(1);
    }

    if (QCoreApplication::instance())
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    return reportRuntimeSnapshotIfEnabled();
}

RuntimeCounterGuard::RuntimeCounterGuard(RuntimeCounter counterValue) : m_counter(counterValue) {
    counter(m_counter).fetch_add(1, std::memory_order_relaxed);
}

RuntimeCounterGuard::~RuntimeCounterGuard() {
    release();
}

RuntimeCounterGuard::RuntimeCounterGuard(RuntimeCounterGuard &&other) noexcept
    : m_counter(other.m_counter), m_active(other.m_active) {
    other.m_active = false;
}

RuntimeCounterGuard &RuntimeCounterGuard::operator=(RuntimeCounterGuard &&other) noexcept {
    if (this == &other)
        return *this;
    release();
    m_counter = other.m_counter;
    m_active = other.m_active;
    other.m_active = false;
    return *this;
}

void RuntimeCounterGuard::release() {
    if (!m_active)
        return;
    counter(m_counter).fetch_sub(1, std::memory_order_relaxed);
    m_active = false;
}

} // namespace fc
