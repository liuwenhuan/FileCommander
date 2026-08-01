#include <gtest/gtest.h>

#include <QLabel>
#include <QRegularExpression>

#include "BreadcrumbBar.h"

namespace {

QStringList linkTargets(const BreadcrumbBar &bar) {
    const auto *label = bar.findChild<QLabel *>();
    if (!label)
        return {};

    QStringList targets;
    QRegularExpression links(QStringLiteral("href=\"([^\"]+)\""));
    auto match = links.globalMatch(label->text());
    while (match.hasNext())
        targets.append(match.next().captured(1));
    return targets;
}

} // namespace

TEST(BreadcrumbBar, WindowsDriveSegmentsKeepDriveRootInEveryTarget) {
    BreadcrumbBar bar;

    bar.setPath(QStringLiteral("C:/Users/deepin/Documents"));

    EXPECT_EQ(linkTargets(bar),
              QStringList({QStringLiteral("C:/"),
                           QStringLiteral("C:/Users"),
                           QStringLiteral("C:/Users/deepin"),
                           QStringLiteral("C:/Users/deepin/Documents")}));
}

TEST(BreadcrumbBar, WindowsDriveRootIsAValidNavigationTarget) {
    BreadcrumbBar bar;

    bar.setPath(QStringLiteral("C:/"));

    EXPECT_EQ(linkTargets(bar), QStringList({QStringLiteral("C:/")}));
}

TEST(BreadcrumbBar, UnixSegmentsRemainAbsolute) {
    BreadcrumbBar bar;

    bar.setPath(QStringLiteral("/home/deepin/Documents"));

    EXPECT_EQ(linkTargets(bar),
              QStringList({QStringLiteral("/"),
                           QStringLiteral("/home"),
                           QStringLiteral("/home/deepin"),
                           QStringLiteral("/home/deepin/Documents")}));
}
