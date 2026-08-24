#include <gtest/gtest.h>

#include <QStringList>

#include "account/EmailAddress.h"

namespace {

TEST(EmailAddress, CanonicalizesAcceptedAddresses) {
    const auto expect = [](const QString &raw, const QString &canonical) {
        const auto result = AccountEmail::canonicalize(raw);
        ASSERT_TRUE(result.has_value()) << raw.toStdString();
        EXPECT_EQ(*result, canonical);
    };

    expect(QStringLiteral(" User.Name+tag@EXAMPLE.com "),
           QStringLiteral("user.name+tag@example.com"));
    expect(QStringLiteral("u@xn--bcher-kva.example"),
           QStringLiteral("u@xn--bcher-kva.example"));
    expect(QStringLiteral("live-abc@example.invalid"),
           QStringLiteral("live-abc@example.invalid"));
    expect(QString(64, QLatin1Char('a')) + QStringLiteral("@example.com"),
           QString(64, QLatin1Char('a')) + QStringLiteral("@example.com"));
}

TEST(EmailAddress, RejectsMalformedAddresses) {
    const QStringList invalid = {
        QString(), QStringLiteral("user"), QStringLiteral("user@localhost"),
        QStringLiteral("a@@example.com"), QStringLiteral("@example.com"),
        QStringLiteral("user@"), QStringLiteral(".user@example.com"),
        QStringLiteral("user..name@example.com"), QStringLiteral("user@example..com"),
        QStringLiteral("user@-example.com"), QStringLiteral("user@example-.com"),
        QStringLiteral("user@exa_mple.com"), QStringLiteral("user name@example.com"),
        QStringLiteral("üser@example.com"), QStringLiteral("user@bücher.example"),
        QStringLiteral("uK@example.com"),
        QString(QChar(0x0085)) + QStringLiteral("user@example.com") + QString(QChar(0x0085)),
        QString(65, QLatin1Char('a')) + QStringLiteral("@example.com")};
    for (const QString &email : invalid)
        EXPECT_FALSE(AccountEmail::canonicalize(email).has_value()) << email.toStdString();
}

} // namespace
