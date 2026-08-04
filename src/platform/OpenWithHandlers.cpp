#include "OpenWithHandlers.h"

#include <QHash>

#include <algorithm>

namespace fc {

namespace {

QString identityOf(const OpenWithHandler &handler) {
    // What the launcher will actually use, in the order it prefers it.
    if (!handler.token.isEmpty())
        return handler.token.toLower();
    if (!handler.program.isEmpty())
        return handler.program.toLower();
    return handler.displayName.toLower();
}

} // namespace

QVector<OpenWithHandler> tidyOpenWithHandlers(QVector<OpenWithHandler> handlers) {
    QVector<OpenWithHandler> unique;
    QHash<QString, int> seen;
    for (const OpenWithHandler &handler : handlers) {
        if (handler.displayName.isEmpty() && handler.program.isEmpty())
            continue;
        const QString identity = identityOf(handler);
        const auto known = seen.constFind(identity);
        if (known != seen.constEnd()) {
            // Keep the first registration's wording, but let a later one
            // promote it: being listed for this type anywhere is what decides
            // which half of the menu it belongs in.
            if (handler.recommended)
                unique[known.value()].recommended = true;
            continue;
        }
        seen.insert(identity, unique.size());
        unique.append(handler);
    }

    std::stable_sort(unique.begin(), unique.end(),
                     [](const OpenWithHandler &left, const OpenWithHandler &right) {
                         if (left.recommended != right.recommended)
                             return left.recommended;
                         return left.displayName.compare(right.displayName, Qt::CaseInsensitive) < 0;
                     });
    return unique;
}

} // namespace fc
