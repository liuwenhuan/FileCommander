#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QSettings>
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
    }

    ~MotionPolicyStateGuard() {
        MotionPolicy::clearReducedForTest();
        MotionPolicy::clearSystemReducedForTest();
        if (m_disableAnimations.isNull())
            qunsetenv("FILECOMMANDER_DISABLE_ANIMATIONS");
        else
            qputenv("FILECOMMANDER_DISABLE_ANIMATIONS", m_disableAnimations);
    }

    QByteArray m_disableAnimations;
};

class ConfigHomeGuard {
public:
    ConfigHomeGuard()
        : m_wasSet(qEnvironmentVariableIsSet("FILECOMMANDER_CONFIG_HOME")),
          m_previous(qgetenv("FILECOMMANDER_CONFIG_HOME")) {
        if (m_dir.isValid())
            qputenv("FILECOMMANDER_CONFIG_HOME", m_dir.path().toUtf8());
    }

    ~ConfigHomeGuard() {
        if (m_wasSet)
            qputenv("FILECOMMANDER_CONFIG_HOME", m_previous);
        else
            qunsetenv("FILECOMMANDER_CONFIG_HOME");
    }

    bool isValid() const { return m_dir.isValid(); }

private:
    QTemporaryDir m_dir;
    bool m_wasSet;
    QByteArray m_previous;
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

TEST(MotionPolicy, LegacyApplicationPreferenceCannotDisableMotion) {
    MotionPolicyStateGuard guard;
    ConfigHomeGuard configHome;
    ASSERT_TRUE(configHome.isValid());
    MotionPolicy::clearReducedForTest();
    MotionPolicy::setSystemReducedForTest(false);
    qputenv("FILECOMMANDER_DISABLE_ANIMATIONS", "0");

    {
        QSettings legacy(Settings::configFilePath(), QSettings::IniFormat);
        legacy.setValue(QStringLiteral("appearance/reduceMotion"), true);
        legacy.sync();
    }

    MainWindow window;
    EXPECT_FALSE(MotionPolicy::reduced());
}

TEST(MotionPolicy, RapidInputSkipsMotion) {
    MotionPolicyStateGuard guard;
    MotionPolicy::setReducedForTest(false);

    EXPECT_TRUE(MotionPolicy::allowFor(InputCadence::Normal));
    EXPECT_FALSE(MotionPolicy::allowFor(InputCadence::Rapid));
}

TEST(MotionPolicy, ConfigurationMenuDoesNotExposeReduceMotion) {
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

    configMenu->popup(QPoint(10, 10));
    QApplication::processEvents();
    configMenu->hide();

    QAction *action = findAction(configMenu, QStringLiteral("Reduce Motion"));
    ASSERT_NE(configMenu->findChild<QAction *>(QStringLiteral("configAutoUpdateAction")), nullptr);
    EXPECT_EQ(action, nullptr);
    EXPECT_EQ(configMenu->findChild<QAction *>(QStringLiteral("configReduceMotionAction")), nullptr);
}

} // namespace
