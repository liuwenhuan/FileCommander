#pragma once

#include <QString>

namespace fc::ProviderPath {

QString normalizeRooted(const QString &path);
QString parent(const QString &path);
QString sibling(const QString &path, const QString &newName);

} // namespace fc::ProviderPath
