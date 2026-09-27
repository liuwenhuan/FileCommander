#include "DirectoryChangeMonitor.h"

#include <cerrno>

#include <QFileInfo>
#include <QSocketNotifier>

#include <fcntl.h>
#include <sys/event.h>
#include <unistd.h>

namespace {

struct MacWatch {
    int queue = -1;
    int directory = -1;
    QSocketNotifier *notifier = nullptr;
};

} // namespace

bool DirectoryChangeMonitor::startNative(const QString &path) {
    const QFileInfo info(path);
    if (!info.exists() || !info.isDir())
        return false;

    auto *state = new MacWatch;
    const QByteArray nativePath = info.absoluteFilePath().toLocal8Bit();
    state->directory = ::open(nativePath.constData(), O_RDONLY | O_EVTONLY | O_CLOEXEC);
    if (state->directory < 0) {
        delete state;
        return false;
    }

    state->queue = ::kqueue();
    if (state->queue < 0) {
        ::close(state->directory);
        delete state;
        return false;
    }

    struct kevent event;
    EV_SET(&event, static_cast<uintptr_t>(state->directory), EVFILT_VNODE,
           EV_ADD | EV_CLEAR,
           NOTE_WRITE | NOTE_DELETE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_LINK |
               NOTE_RENAME | NOTE_REVOKE,
           0, nullptr);
    if (::kevent(state->queue, &event, 1, nullptr, 0, nullptr) < 0) {
        ::close(state->queue);
        ::close(state->directory);
        delete state;
        return false;
    }

    m_nativeState = state;
    state->notifier = new QSocketNotifier(state->queue, QSocketNotifier::Read, this);
    connect(state->notifier, &QSocketNotifier::activated, this, [this](int) {
        auto *watch = static_cast<MacWatch *>(m_nativeState);
        if (!watch)
            return;

        bool changed = false;
        for (;;) {
            struct kevent event;
            const timespec timeout{0, 0};
            const int count = ::kevent(watch->queue, nullptr, 0, &event, 1, &timeout);
            if (count == 0)
                break;
            if (count < 0) {
                if (errno == EINTR)
                    continue;
                requireReconciliation();
                return;
            }

            if ((event.flags & EV_ERROR) ||
                (event.fflags & (NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE))) {
                requireReconciliation();
                return;
            }
            changed = true;
        }

        if (changed)
            notifyChanged();
    });
    return true;
}

void DirectoryChangeMonitor::stopNative() {
    auto *state = static_cast<MacWatch *>(m_nativeState);
    if (!state)
        return;
    m_nativeState = nullptr;

    if (state->notifier) {
        state->notifier->setEnabled(false);
        delete state->notifier;
    }
    if (state->queue >= 0)
        ::close(state->queue);
    if (state->directory >= 0)
        ::close(state->directory);
    delete state;
}
