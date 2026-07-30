#include <gtest/gtest.h>

#include <QAction>
#include <QMenu>
#include <QTemporaryDir>

#include "MainWindow.h"
#include "MotionPolicy.h"
#include "Settings.h"

namespace {

class MotionPolicyStateGuard {
public:
    MotionPolicyStateGuard() {
        m_disableAnimations = qgetenv("FILECOMMANDER_DISABLE_ANIMATIONS");
        MotionPolicy::clearReducedForTest();
        MotionPolicy::clearSystemReducedForTest();
        MotionPolicy::setApplicationReduced(false);
    }

    ~MotionPolicyStateGuard() {
        MotionPolicy::clearReducedForTest();
        MotionPolicy::clearSystemReducedForTest();
        MotionPolicy::setApplicationReduced(false);
        if (m_disableAnimations.isNull())
            qunsetenv("FILECOMMANDER_DISABLE_ANIMATIONS");
        else
            qputenv("FILECOMMANDER_DISABLE_ANIMATIONS", m_disableAnimations);
    }

    QByteArray m_disableAnimations;
};

QAction *findAction(QMenu *menu, const QString &text) {
    if (!menu)
        return nullptr;
    for (QAction *action : menu->actions()) {
        if (action->text().section(QLatin1Char('\t'), 0, 0) == text)
            return action;
        if (QMenu *subMenu = action->menu()) {
            if (QAction *found = findAction(subMenu, text))
                return found;
        }
    }
    return nullptr;
}

TEST(MotionPolicy, ReturnsSpecifiedDurationsAndEaseOutCurve) {
    MotionPolicyStateGuard guard;
    MotionPolicy::setReducedForTest(false);

    EXPECT_EQ(MotionPolicy::duration(MotionDuration::Fast), 100);
    EXPECT_EQ(MotionPolicy::duration(MotionDuration::Normal), 150);
    EXPECT_EQ(MotionPolicy::duration(MotionDuration::Slow), 200);
    EXPECT_EQ(MotionPolicy::easing().type(), QEasingCurve::OutCubic);
}

TEST(MotionPolicy, ExplicitReducedOverrideDisablesMotion) {
    MotionPolicyStateGuard guard;
    MotionPolicy::setReducedForTest(true);

    EXPECT_TRUE(MotionPolicy::reduced());
    EXPECT_EQ(MotionPolicy::duration(MotionDuration::Normal), 0);
    EXPECT_FALSE(MotionPolicy::allowFor(InputCadence::Normal));
}

TEST(MotionPolicy, DisableAnimationsEnvironmentDisablesMotion) {
    MotionPolicyStateGuard guard;
    qputenv("FILECOMMANDER_DISABLE_ANIMATIONS", "1");

    EXPECT_TRUE(MotionPolicy::reduced());
    EXPECT_EQ(MotionPolicy::duration(MotionDuration::Normal), 0);
}

TEST(MotionPolicy, SystemPreferenceTestOverrideIsDeterministic) {
    MotionPolicyStateGuard guard;
    qputenv("FILECOMMANDER_DISABLE_ANIMATIONS", "0");

    MotionPolicy::setSystemReducedForTest(true);
    EXPECT_TRUE(MotionPolicy::reduced());

    MotionPolicy::setSystemReducedForTest(false);
    EXPECT_FALSE(MotionPolicy::reduced());
}

TEST(MotionPolicy, ApplicationPreferenceUpdatesPolicyImmediately) {
    MotionPolicyStateGuard guard;
    MotionPolicy::clearReducedForTest();
    MotionPolicy::setSystemReducedForTest(false);
    qputenv("FILECOMMANDER_DISABLE_ANIMATIONS", "0");

    EXPECT_FALSE(MotionPolicy::reduced());
    MotionPolicy::setApplicationReduced(true);
    EXPECT_TRUE(MotionPolicy::reduced());

    MotionPolicy::setApplicationReduced(false);
    EXPECT_FALSE(MotionPolicy::reduced());
}

TEST(MotionPolicy, RapidInputSkipsMotion) {
    MotionPolicyStateGuard guard;
    MotionPolicy::setReducedForTest(false);

    EXPECT_TRUE(MotionPolicy::allowFor(InputCadence::Normal));
    EXPECT_FALSE(MotionPolicy::allowFor(InputCadence::Rapid));
}

TEST(MotionPolicy, PersistsReducedMotionPreference) {
    MotionPolicyStateGuard guard;
    QTemporaryDir temporaryDir;
    ASSERT_TRUE(temporaryDir.isValid());
    const QString settingsPath = temporaryDir.filePath(QStringLiteral("settings.ini"));

    {
        Settings writer(settingsPath);
        EXPECT_FALSE(writer.reduceMotion());
        writer.setReduceMotion(true);
    }

    Settings reader(settingsPath);
    EXPECT_TRUE(reader.reduceMotion());
}

TEST(MotionPolicy, ReduceMotionActionUpdatesPolicyImmediately) {
    MotionPolicyStateGuard guard;
    qputenv("FILECOMMANDER_DISABLE_ANIMATIONS", "0");
    MainWindow window;

    QMenu *configMenu = nullptr;
    for (QMenu *menu : window.findChildren<QMenu *>()) {
        if (menu->title() == QStringLiteral("Con&fig")) {
            configMenu = menu;
            break;
        }
    }
    ASSERT_NE(configMenu, nullptr);

    QAction *action = findAction(configMenu, QStringLiteral("Reduce Motion"));
    ASSERT_NE(action, nullptr);
    EXPECT_TRUE(action->isCheckable());

    const bool originallyEnabled = action->isChecked();
    if (originallyEnabled)
        action->trigger();

    action->trigger();
    EXPECT_TRUE(action->isChecked());
    EXPECT_TRUE(MotionPolicy::reduced());

    action->trigger();
    if (originallyEnabled)
        action->trigger();
}

} // namespace
