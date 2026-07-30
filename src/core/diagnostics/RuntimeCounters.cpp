#include "RuntimeCounters.h"

#include <QDebug>

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

void reportRuntimeSnapshotIfEnabled() {
    if (qEnvironmentVariableIntValue("FILECOMMANDER_DIAGNOSTICS") != 1)
        return;
    static std::atomic_flag reported = ATOMIC_FLAG_INIT;
    if (reported.test_and_set(std::memory_order_relaxed))
        return;

    const RuntimeSnapshot snapshot = runtimeSnapshot();
    qInfo().nospace() << "FileCommander RuntimeSnapshot"
                      << " networkSessions=" << snapshot.networkSessions
                      << " networkThreads=" << snapshot.networkThreads
                      << " activeHeartbeats=" << snapshot.activeHeartbeats
                      << " transferWorkers=" << snapshot.transferWorkers
                      << " curlTransfers=" << snapshot.curlTransfers;
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
