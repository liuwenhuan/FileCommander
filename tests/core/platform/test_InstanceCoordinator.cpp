#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

#include <chrono>
#include <future>

#include "InstanceCoordinator.h"

TEST(InstanceCoordinatorTest, SecondLaunchForwardsArgumentsToPrimaryInstance) {
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString serverName = QStringLiteral("FileCommander-test-") +
                               QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QStringList arguments{QStringLiteral("FileCommander"), temp.path()};

    InstanceCoordinator primary(serverName);
    ASSERT_EQ(primary.startOrActivate({QStringLiteral("FileCommander")}),
              InstanceCoordinator::StartResult::Primary);
    ASSERT_TRUE(primary.isPrimary());
    QSignalSpy activations(&primary, &InstanceCoordinator::activationRequested);

    auto second = std::async(std::launch::async, [serverName, arguments] {
        InstanceCoordinator instance(serverName);
        return instance.startOrActivate(arguments);
    });
    QTRY_VERIFY(second.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
    EXPECT_EQ(second.get(), InstanceCoordinator::StartResult::Forwarded);
    QTRY_COMPARE(activations.count(), 1);
    EXPECT_EQ(activations.takeFirst().at(0).toStringList(), arguments);
}
