#pragma once

#include <QString>

namespace ttc {

// Fires a best-effort passive desktop notification. A silent no-op where there
// is no notification service or the platform refuses the request: an arrival
// must never cost the user a modal or a crash.
void notify(const QString &title, const QString &body);

} // namespace ttc
