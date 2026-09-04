#define NOMINMAX
#include "DirectoryChangeMonitor.h"

#include <windows.h>

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QWinEventNotifier>

namespace {

struct WindowsWatch {
    HANDLE directory = INVALID_HANDLE_VALUE;
    HANDLE event = nullptr;
    OVERLAPPED overlapped{};
    QByteArray buffer = QByteArray(64 * 1024, Qt::Uninitialized);
    QWinEventNotifier *notifier = nullptr;
};

QString nativePath(const QString &path) {
    return QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
}

} // namespace

bool DirectoryChangeMonitor::startNative(const QString &path) {
    const QFileInfo info(path);
    if (!info.exists() || !info.isDir())
        return false;

    const QString native = nativePath(path);
    auto *state = new WindowsWatch;
    state->directory = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()),
                                   FILE_LIST_DIRECTORY,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                                   nullptr);
    if (state->directory == INVALID_HANDLE_VALUE) {
        delete state;
        return false;
    }

    state->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!state->event) {
        CloseHandle(state->directory);
        delete state;
        return false;
    }
    state->overlapped.hEvent = state->event;
    m_nativeState = state;
    state->notifier = new QWinEventNotifier(state->event, this);
    connect(state->notifier, &QWinEventNotifier::activated, this,
            [this](HANDLE) {
                auto *watch = static_cast<WindowsWatch *>(m_nativeState);
                if (!watch)
                    return;
                DWORD bytes = 0;
                if (!GetOverlappedResult(watch->directory, &watch->overlapped, &bytes,
                                         FALSE)) {
                    const DWORD error = GetLastError();
                    if (error != ERROR_IO_INCOMPLETE)
                        requireReconciliation();
                    return;
                }
                if (bytes == 0) {
                    requireReconciliation();
                    return;
                }
                notifyChanged();

                ResetEvent(watch->event);
                ZeroMemory(&watch->overlapped, sizeof(watch->overlapped));
                watch->overlapped.hEvent = watch->event;
                const DWORD notifyFilter = FILE_NOTIFY_CHANGE_FILE_NAME |
                                            FILE_NOTIFY_CHANGE_DIR_NAME |
                                            FILE_NOTIFY_CHANGE_ATTRIBUTES |
                                            FILE_NOTIFY_CHANGE_SIZE |
                                            FILE_NOTIFY_CHANGE_LAST_WRITE |
                                            FILE_NOTIFY_CHANGE_CREATION;
                if (!ReadDirectoryChangesW(watch->directory, watch->buffer.data(),
                                           static_cast<DWORD>(watch->buffer.size()), FALSE,
                                           notifyFilter, nullptr, &watch->overlapped,
                                           nullptr) &&
                    GetLastError() != ERROR_IO_PENDING)
                    requireReconciliation();
            });
    state->notifier->setEnabled(true);

    const DWORD notifyFilter = FILE_NOTIFY_CHANGE_FILE_NAME |
                                FILE_NOTIFY_CHANGE_DIR_NAME |
                                FILE_NOTIFY_CHANGE_ATTRIBUTES |
                                FILE_NOTIFY_CHANGE_SIZE |
                                FILE_NOTIFY_CHANGE_LAST_WRITE |
                                FILE_NOTIFY_CHANGE_CREATION;
    if (!ReadDirectoryChangesW(state->directory, state->buffer.data(),
                               static_cast<DWORD>(state->buffer.size()), FALSE,
                               notifyFilter, nullptr, &state->overlapped, nullptr) &&
        GetLastError() != ERROR_IO_PENDING) {
        stopNative();
        return false;
    }
    return true;
}

void DirectoryChangeMonitor::stopNative() {
    auto *state = static_cast<WindowsWatch *>(m_nativeState);
    if (!state)
        return;
    m_nativeState = nullptr;
    if (state->notifier) {
        state->notifier->setEnabled(false);
        delete state->notifier;
        state->notifier = nullptr;
    }
    if (state->directory != INVALID_HANDLE_VALUE) {
        CancelIoEx(state->directory, &state->overlapped);
        CloseHandle(state->directory);
    }
    if (state->event)
        CloseHandle(state->event);
    delete state;
}
