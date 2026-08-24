#include <gtest/gtest.h>

#include <QApplication>
#include <QDate>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>

#include "MainWindow.h"
#include "Settings.h"
#include "ThemeStateGuard.h"
#include "TitleBar.h"
#include "dialogs/UpdateDialog.h"
#include "update/UpdateChecker.h"
#include "version.h"

namespace {
class EnvironmentGuard {
public:
    EnvironmentGuard(const char *name, const QByteArray &value)
        : m_name(name), m_old(qgetenv(name)), m_had(qEnvironmentVariableIsSet(name)) { qputenv(name, value); }
    ~EnvironmentGuard() { if (m_had) qputenv(m_name, m_old); else qunsetenv(m_name); }
private:
    QByteArray m_name, m_old;
    bool m_had;
};

UpdateInfo sampleRelease() {
    return {QStringLiteral("9.9.9"), QStringLiteral("first line\nsecond line"), QStringLiteral("2026-08-24")};
}
} // namespace

TEST(UpdateDialogTest, ShowsReleaseAndOnlyTheFixedUpdatePage) {
    ThemeStateGuard theme;
    UpdateDialog dialog(sampleRelease());
    auto *notes = dialog.findChild<QTextEdit *>();
    auto *page = dialog.findChild<QLineEdit *>(QStringLiteral("UpdatePageUrl"));
    auto *open = dialog.findChild<QPushButton *>(QStringLiteral("UpdatePageButton"));
    ASSERT_NE(notes, nullptr);
    ASSERT_NE(page, nullptr);
    ASSERT_NE(open, nullptr);
    EXPECT_EQ(notes->toPlainText(), QStringLiteral("first line\nsecond line"));
    EXPECT_EQ(page->text(), UpdateChecker::updatePageUrl());
    EXPECT_EQ(dialog.updatePageText(), UpdateChecker::updatePageUrl());
    EXPECT_EQ(dialog.findChild<QLineEdit *>(QStringLiteral("UpdateDownloadUrl")), nullptr);
    EXPECT_EQ(dialog.findChild<QLineEdit *>(QStringLiteral("UpdateChecksum")), nullptr);
}

TEST(UpdateBadgeTest, ShowsOnlyWhenThereIsAnOffer) {
    ThemeStateGuard theme;
    MainWindow window;
    auto *title = window.findChild<TitleBar *>();
    ASSERT_NE(title, nullptr);
    auto *badge = title->findChild<QToolButton *>(QStringLiteral("UpdateBadge"));
    ASSERT_NE(badge, nullptr);
    EXPECT_TRUE(badge->isHidden());
    title->setUpdateAvailable(true);
    EXPECT_FALSE(badge->isHidden());
}

TEST(ScheduledUpdateCheckTest, IsDueOncePerCalendarDayRegardlessOfLegacyOptOut) {
    ThemeStateGuard theme;
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    EnvironmentGuard config("FILECOMMANDER_CONFIG_HOME", temporary.path().toUtf8());
    Settings settings;
    settings.setUpdateLastCheckDate(QString());
    MainWindow window;
    EXPECT_TRUE(window.updateCheckIsDue());
    settings.setUpdateLastCheckDate(QDate::currentDate().toString(Qt::ISODate));
    EXPECT_FALSE(window.updateCheckIsDue());
    settings.setUpdateLastCheckDate(QDate::currentDate().addDays(-1).toString(Qt::ISODate));
    EXPECT_TRUE(window.updateCheckIsDue());
}

TEST(ScheduledUpdateCheckTest, KeepsHourlyCalendarTimer) {
    ThemeStateGuard theme;
    MainWindow window;
    auto *timer = window.findChild<QTimer *>(QStringLiteral("ScheduledUpdateCheck"));
    ASSERT_NE(timer, nullptr);
    EXPECT_TRUE(timer->isActive());
    EXPECT_EQ(timer->interval(), MainWindow::kUpdateCheckTickMs);
}
