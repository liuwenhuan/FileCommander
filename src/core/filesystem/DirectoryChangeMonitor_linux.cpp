#include "DirectoryChangeMonitor.h"

#include <cerrno>
#include <cstring>

#include <QFileInfo>
#include <QSocketNotifier>

#include <sys/inotify.h>
#include <unistd.h>

namespace {

struct LinuxWatch {
    int fd = -1;
    int watch = -1;
    QSocketNotifier *notifier = nullptr;
};

} // namespace

bool DirectoryChangeMonitor::startNative(const QString &path) {
    const QFileInfo info(path);
    if (!info.exists() || !info.isDir())
        return false;

    auto *state = new LinuxWatch;
    state->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (state->fd < 0) {
        delete state;
        return false;
    }
    const QByteArray encoded = info.absoluteFilePath().toLocal8Bit();
    constexpr uint32_t mask = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
                               IN_CLOSE_WRITE | IN_MODIFY | IN_ATTRIB | IN_DELETE_SELF |
                               IN_MOVE_SELF;
    state->watch = inotify_add_watch(state->fd, encoded.constData(), mask);
    if (state->watch < 0) {
        close(state->fd);
        delete state;
        return false;
    }

    m_nativeState = state;
    state->notifier = new QSocketNotifier(state->fd, QSocketNotifier::Read, this);
    connect(state->notifier, &QSocketNotifier::activated, this,
            [this](int) {
                auto *watch = static_cast<LinuxWatch *>(m_nativeState);
                if (!watch)
                    return;
                alignas(inotify_event) char buffer[64 * 1024];
                bool changed = false;
                for (;;) {
                    const ssize_t count = read(watch->fd, buffer, sizeof(buffer));
                    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                        break;
                    if (count <= 0) {
                        if (count == 0 || (errno != EINTR && errno != EAGAIN &&
                                           errno != EWOULDBLOCK))
                            requireReconciliation();
                        break;
                    }
                    ssize_t offset = 0;
                    while (offset < count) {
                        const auto *event = reinterpret_cast<const inotify_event *>(
                            buffer + offset);
                        if (event->mask & (IN_Q_OVERFLOW | IN_IGNORED | IN_DELETE_SELF |
                                           IN_MOVE_SELF)) {
                            requireReconciliation();
                            return;
                        }
                        changed = true;
                        offset += sizeof(inotify_event) + event->len;
                    }
                }
                if (changed)
                    notifyChanged();
            });
    return true;
}

void DirectoryChangeMonitor::stopNative() {
    auto *state = static_cast<LinuxWatch *>(m_nativeState);
    if (!state)
        return;
    m_nativeState = nullptr;
    if (state->notifier) {
        state->notifier->setEnabled(false);
        delete state->notifier;
    }
    if (state->watch >= 0)
        inotify_rm_watch(state->fd, state->watch);
    if (state->fd >= 0)
        close(state->fd);
    delete state;
}

