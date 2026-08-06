#include "StartupTrace.h"

void StartupTrace::mark(const QString &name, qint64 elapsedMs) {
    if (!m_collecting)
        return;
    m_phases.append({name, elapsedMs});
}

QJsonObject StartupTrace::toJson() const {
    QJsonObject object;
    for (const auto &phase : m_phases)
        object.insert(phase.first, phase.second);
    return object;
}
