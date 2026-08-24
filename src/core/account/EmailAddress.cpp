#include "EmailAddress.h"

#include <QStringList>

namespace {

bool isAsciiWhitespace(QChar character) {
    const ushort value = character.unicode();
    return value == 0x20 || (value >= 0x09 && value <= 0x0d);
}

QString trimmedAscii(const QString &raw) {
    int first = 0;
    int last = raw.size();
    while (first < last && isAsciiWhitespace(raw.at(first)))
        ++first;
    while (last > first && isAsciiWhitespace(raw.at(last - 1)))
        --last;
    return raw.mid(first, last - first);
}

bool isAsciiAlnum(QChar character) {
    return (character >= QLatin1Char('a') && character <= QLatin1Char('z')) ||
           (character >= QLatin1Char('A') && character <= QLatin1Char('Z')) ||
           (character >= QLatin1Char('0') && character <= QLatin1Char('9'));
}

bool isLocalCharacter(QChar character) {
    return isAsciiAlnum(character) ||
           QStringLiteral("!#$%&'*+/=?^_`{|}~.-").contains(character);
}

bool validDomain(const QString &domain) {
    const QStringList labels = domain.split(QLatin1Char('.'), Qt::KeepEmptyParts);
    if (labels.size() < 2)
        return false;
    for (const QString &label : labels) {
        if (label.isEmpty() || label.size() > 63 || !isAsciiAlnum(label.front()) ||
            !isAsciiAlnum(label.back())) {
            return false;
        }
        for (const QChar character : label)
            if (!isAsciiAlnum(character) && character != QLatin1Char('-'))
                return false;
    }
    return true;
}

} // namespace

std::optional<QString> AccountEmail::canonicalize(const QString &raw) {
    const QString trimmed = trimmedAscii(raw);
    if (trimmed.isEmpty() || trimmed.size() > 254)
        return std::nullopt;
    for (const QChar character : trimmed)
        if (character.unicode() > 0x7f)
            return std::nullopt;
    const QString email = trimmed.toLower();

    const int at = email.indexOf(QLatin1Char('@'));
    if (at <= 0 || at != email.lastIndexOf(QLatin1Char('@')))
        return std::nullopt;

    const QString local = email.left(at);
    const QString domain = email.mid(at + 1);
    if (local.size() > 64 || local.front() == QLatin1Char('.') ||
        local.back() == QLatin1Char('.') || local.contains(QStringLiteral(".."))) {
        return std::nullopt;
    }
    for (const QChar character : local)
        if (!isLocalCharacter(character))
            return std::nullopt;

    return validDomain(domain) ? std::optional<QString>(email) : std::nullopt;
}
