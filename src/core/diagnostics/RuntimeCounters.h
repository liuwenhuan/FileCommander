#pragma once

namespace fc {

enum class RuntimeCounter {
    NetworkSession,
    NetworkThread,
    ActiveHeartbeat,
    TransferWorker,
    CurlTransfer,
};

struct RuntimeSnapshot {
    int networkSessions = 0;
    int networkThreads = 0;
    int activeHeartbeats = 0;
    int transferWorkers = 0;
    int curlTransfers = 0;
};

RuntimeSnapshot runtimeSnapshot();
bool reportRuntimeSnapshotIfEnabled();
bool reportFinalRuntimeSnapshotIfEnabled(int timeoutMs = 1000);

class RuntimeCounterGuard final {
public:
    explicit RuntimeCounterGuard(RuntimeCounter counter);
    ~RuntimeCounterGuard();

    RuntimeCounterGuard(const RuntimeCounterGuard &) = delete;
    RuntimeCounterGuard &operator=(const RuntimeCounterGuard &) = delete;

    RuntimeCounterGuard(RuntimeCounterGuard &&other) noexcept;
    RuntimeCounterGuard &operator=(RuntimeCounterGuard &&other) noexcept;

private:
    void release();

    RuntimeCounter m_counter;
    bool m_active = true;
};

} // namespace fc
