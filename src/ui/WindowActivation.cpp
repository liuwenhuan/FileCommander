#include "WindowActivation.h"

#include <QTimer>
#include <QWidget>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace ttc {
namespace {

class QtWindowActivationBackend final : public WindowActivationBackend {
public:
    void showNormal(QWidget *window) override { window->showNormal(); }
    void raise(QWidget *window) override { window->raise(); }
    void activate(QWidget *window) override { window->activateWindow(); }
    void nativeActivate(QWidget *window) override {
#ifdef Q_OS_WIN
        const HWND hwnd = reinterpret_cast<HWND>(window->winId());
        if (!hwnd)
            return;
        ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOWNORMAL);

        const DWORD currentThread = GetCurrentThreadId();
        const HWND foreground = GetForegroundWindow();
        DWORD foregroundProcess = 0;
        const DWORD foregroundThread =
            foreground ? GetWindowThreadProcessId(foreground, &foregroundProcess) : 0;
        DWORD targetProcess = 0;
        const DWORD targetThread = GetWindowThreadProcessId(hwnd, &targetProcess);

        const bool attachForeground =
            foregroundThread != 0 && foregroundThread != currentThread;
        const bool attachTarget = targetThread != 0 && targetThread != currentThread;
        if (attachForeground)
            AttachThreadInput(currentThread, foregroundThread, TRUE);
        if (attachTarget)
            AttachThreadInput(currentThread, targetThread, TRUE);

        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER);
        SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER);
        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
        SetActiveWindow(hwnd);
        SetFocus(hwnd);

        if (attachTarget)
            AttachThreadInput(currentThread, targetThread, FALSE);
        if (attachForeground)
            AttachThreadInput(currentThread, foregroundThread, FALSE);
#else
        Q_UNUSED(window);
#endif
    }
    void schedule(QWidget *window, int delayMs, std::function<void()> callback) override {
        QTimer::singleShot(delayMs, window, [callback = std::move(callback)] { callback(); });
    }
};

void activateOnce(QWidget *window, WindowActivationBackend &backend) {
    if (!window)
        return;
    backend.showNormal(window);
    backend.raise(window);
    backend.activate(window);
    backend.nativeActivate(window);
}

} // namespace

QVector<int> foregroundActivationRetryDelays() {
    return {0, 150, 600, 1500};
}

void requestWindowForeground(QWidget *window) {
    static QtWindowActivationBackend backend;
    requestWindowForeground(window, backend);
}

void requestWindowForeground(QWidget *window, WindowActivationBackend &backend) {
    if (!window)
        return;

    activateOnce(window, backend);
    for (int delayMs : foregroundActivationRetryDelays()) {
        backend.schedule(window, delayMs, [window, &backend] { activateOnce(window, backend); });
    }
}

} // namespace ttc
