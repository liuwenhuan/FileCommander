#pragma once

#include <QString>

namespace ttc {

// Fires a best-effort passive desktop notification. A silent no-op where there
// is no notification service (headless, a platform without
// org.freedesktop.Notifications, or a bus that refuses): an arrival must never
// cost the user a modal or a crash. Linux only for now; other platforms are a
// no-op until they get a native path.
void notify(const QString &title, const QString &body);

} // namespace ttc
