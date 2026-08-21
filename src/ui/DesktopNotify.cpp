#include "DesktopNotify.h"

#include <QStringList>
#include <QVariantMap>

#if FILECOMMANDER_HAS_LINUX_INTEGRATION
#include <QDBusInterface>
#endif

namespace ttc {

void notify(const QString &title, const QString &body) {
#if FILECOMMANDER_HAS_LINUX_INTEGRATION
    // org.freedesktop.Notifications is the freedesktop desktop-notification
    // spec; every mainstream Linux desktop ships it. Fire-and-forget: no reply
    // is read, and a missing service or bus is simply no notification.
    QDBusInterface iface(QStringLiteral("org.freedesktop.Notifications"),
                         QStringLiteral("/org/freedesktop/Notifications"),
                         QStringLiteral("org.freedesktop.Notifications"),
                         QDBusConnection::sessionBus());
    if (!iface.isValid())
        return;
    const QStringList actions;
    const QVariantMap hints;
    iface.call(QDBus::NoBlock, QStringLiteral("Notify"),
               QStringLiteral("FileCommander"), static_cast<quint32>(0), QString(),
               title, body, actions, hints, static_cast<int>(5000));
#else
    Q_UNUSED(title);
    Q_UNUSED(body);
#endif
}

} // namespace ttc
