#pragma once

#include <optional>

#include <QString>

namespace AccountEmail {

// Returns the trimmed, ASCII-lowercase account identity when it meets the
// account server's deliberately syntax-only mailbox contract.
std::optional<QString> canonicalize(const QString &raw);

} // namespace AccountEmail
