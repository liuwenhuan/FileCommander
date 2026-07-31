#pragma once

#include <QVector>

#include <functional>

class QWidget;

namespace ttc {

class WindowActivationBackend {
public:
    virtual ~WindowActivationBackend() = default;
    virtual void showNormal(QWidget *window) = 0;
    virtual void raise(QWidget *window) = 0;
    virtual void activate(QWidget *window) = 0;
    virtual void nativeActivate(QWidget *window) = 0;
    virtual void schedule(QWidget *window, int delayMs, std::function<void()> callback) = 0;
};

QVector<int> foregroundActivationRetryDelays();
void requestWindowForeground(QWidget *window);
void requestWindowForeground(QWidget *window, WindowActivationBackend &backend);

} // namespace ttc
