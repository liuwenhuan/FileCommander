#include "DirectoryChangeMonitor.h"

bool DirectoryChangeMonitor::startNative(const QString &path) {
    Q_UNUSED(path);
    return false;
}

void DirectoryChangeMonitor::stopNative() {}

