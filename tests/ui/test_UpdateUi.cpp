#include <gtest/gtest.h>

#include <QApplication>
#include <QDate>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>

#include "MainWindow.h"
#include "Settings.h"
#include "TitleBar.h"
#include "dialogs/UpdateDialog.h"
#include "update/UpdateChecker.h"

#include "version.h"

// The user-facing half of the online updater: the badge that says a release
// exists, and the dialog that applies it. Neither reaches the network here --
// what is being checked is that the states the updater can report are all
// rendered, including the ones that only happen when something goes wrong.
namespace {

QPushButton *buttonWithText(QWidget &parent, const QString &text) {
    for (QPushButton *button : parent.findChildren<QPushButton *>())
        if (button->text() == text)
            return button;
    return nullptr;
}

// Points the settings at a throwaway directory so a test can set
// update/lastCheckDate without editing the developer's own config.
class EnvironmentGuard final {
public:
    EnvironmentGuard(const char *name, const QByteArray &value)
        : m_name(name), m_original(qgetenv(name)), m_hadOriginal(qEnvironmentVariableIsSet(name)) {
        qputenv(name, value);
    }
    ~EnvironmentGuard() {
        if (m_hadOriginal)
            qputenv(m_name.constData(), m_original);
        else
            qunsetenv(m_name.constData());
    }

private:
    QByteArray m_name;
    QByteArray m_original;
    bool m_hadOriginal;
};

UpdateInfo sampleRelease() {
    UpdateInfo info;
    info.version = QStringLiteral("9.9.9");
    info.date = QStringLiteral("2026-08-03");
    info.notes = QStringLiteral("first line\nsecond line");
    info.url = QStringLiteral("https://example.invalid/FileCommander-9.9.9.zip");
    info.sha256 = QString(64, QLatin1Char('b'));
    return info;
}

} // namespace

TEST(UpdateDialogTest, ShowsTheReleaseAgainstTheRunningVersion) {
    UpdateDialog dialog(sampleRelease());

    bool sawHeadline = false;
    bool sawDate = false;
    for (QLabel *label : dialog.findChildren<QLabel *>()) {
        if (label->text().contains(QStringLiteral("9.9.9"))
            && label->text().contains(QStringLiteral(TTC_VERSION)))
            sawHeadline = true;
        if (label->text().contains(QStringLiteral("2026-08-03")))
            sawDate = true;
    }
    EXPECT_TRUE(sawHeadline) << "the dialog must say which version replaces which";
    EXPECT_TRUE(sawDate);

    auto *notes = dialog.findChild<QTextEdit *>();
    ASSERT_NE(notes, nullptr);
    EXPECT_EQ(notes->toPlainText(), QStringLiteral("first line\nsecond line"));
    EXPECT_TRUE(notes->isReadOnly());

    auto *progress = dialog.findChild<QProgressBar *>();
    ASSERT_NE(progress, nullptr);
    EXPECT_TRUE(progress->isHidden()) << "no progress bar before anything is downloading";
}

TEST(UpdateDialogTest, ReleaseWithoutNotesStillSaysSomething) {
    UpdateInfo info = sampleRelease();
    info.notes.clear();
    UpdateDialog dialog(info);

    auto *notes = dialog.findChild<QTextEdit *>();
    ASSERT_NE(notes, nullptr);
    EXPECT_FALSE(notes->toPlainText().isEmpty()) << "an empty notes box looks like a broken dialog";
}

TEST(UpdateDialogTest, LaterDismissesWithoutStartingAnything) {
    UpdateDialog dialog(sampleRelease());
    QPushButton *later = buttonWithText(dialog, QStringLiteral("Later"));
    ASSERT_NE(later, nullptr);
    QSignalSpy restart(&dialog, &UpdateDialog::restartRequested);

    later->click();

    EXPECT_FALSE(dialog.isVisible());
    EXPECT_EQ(dialog.result(), int(QDialog::Rejected));
    EXPECT_EQ(restart.count(), 0);
}

// The failure path is the one a user is most likely to meet (a server that is
// down, a package that has moved) and the one that most needs a way out. An
// UpdateInfo with no URL fails inside apply() without any network, which is
// exactly the state machine under test.
TEST(UpdateDialogTest, AFailedAttemptOffersARetryAndReleasesTheButtons) {
    UpdateInfo info = sampleRelease();
    info.url.clear(); // apply() rejects this immediately
    UpdateDialog dialog(info);

    QPushButton *confirm = buttonWithText(dialog, QStringLiteral("Update Now"));
    QPushButton *later = buttonWithText(dialog, QStringLiteral("Later"));
    ASSERT_NE(confirm, nullptr);
    ASSERT_NE(later, nullptr);
    QSignalSpy restart(&dialog, &UpdateDialog::restartRequested);

    confirm->click();
    qApp->processEvents();

    EXPECT_EQ(restart.count(), 0) << "a failed update must not ask the app to restart";
    QPushButton *retry = buttonWithText(dialog, QStringLiteral("Retry"));
    ASSERT_NE(retry, nullptr) << "no way to try again after a failure";
    EXPECT_TRUE(retry->isEnabled());
    EXPECT_TRUE(later->isEnabled()) << "the user must still be able to walk away";
    EXPECT_EQ(later->text(), QStringLiteral("Later"))
        << "the cancel button should go back to meaning 'dismiss' once nothing is running";

    auto *progress = dialog.findChild<QProgressBar *>();
    ASSERT_NE(progress, nullptr);
    EXPECT_TRUE(progress->isHidden());

    bool statusShown = false;
    for (QLabel *label : dialog.findChildren<QLabel *>())
        if (label->text().contains(QStringLiteral("incomplete"), Qt::CaseInsensitive))
            statusShown = true;
    EXPECT_TRUE(statusShown) << "the dialog must say why it failed";
}

// --- the badge -------------------------------------------------------------

TEST(UpdateBadgeTest, StaysHiddenUntilThereIsSomethingToOffer) {
    MainWindow window;
    auto *titleBar = window.findChild<TitleBar *>();
    ASSERT_NE(titleBar, nullptr);
    auto *badge = titleBar->findChild<QToolButton *>(QStringLiteral("UpdateBadge"));
    ASSERT_NE(badge, nullptr);

    EXPECT_TRUE(badge->isHidden()) << "a fresh window must not claim an update exists";

    titleBar->setUpdateAvailable(true);
    EXPECT_FALSE(badge->isHidden());
    titleBar->setUpdateAvailable(false);
    EXPECT_TRUE(badge->isHidden());
}

TEST(UpdateBadgeTest, ClickingItAsksTheWindowToOpenTheUpdate) {
    MainWindow window;
    auto *titleBar = window.findChild<TitleBar *>();
    ASSERT_NE(titleBar, nullptr);
    auto *badge = titleBar->findChild<QToolButton *>(QStringLiteral("UpdateBadge"));
    ASSERT_NE(badge, nullptr);
    QSignalSpy requested(titleBar, &TitleBar::updateRequested);

    titleBar->setUpdateAvailable(true);
    badge->click();

    EXPECT_EQ(requested.count(), 1);
}

// --- the scheduled check ---------------------------------------------------

// The policy is "once per calendar day", and it has to keep holding for a
// window that is never closed. A machine that suspends every evening is exactly
// the case a plain 24-hour timer gets wrong, so the question is asked against
// the date, repeatedly, rather than answered once at startup.
TEST(ScheduledUpdateCheckTest, IsDueOncePerDayAndOnlyWhenAutomaticCheckingIsOn) {
    QTemporaryDir configDir;
    ASSERT_TRUE(configDir.isValid());
    EnvironmentGuard config("FILECOMMANDER_CONFIG_HOME", configDir.path().toUtf8());

    MainWindow window;
    Settings settings;
    const QString today = QDate::currentDate().toString(Qt::ISODate);

    settings.setAutoUpdateCheck(true);
    settings.setUpdateLastCheckDate(QString());
    EXPECT_TRUE(window.updateCheckIsDue()) << "never checked -> due";

    settings.setUpdateLastCheckDate(today);
    EXPECT_FALSE(window.updateCheckIsDue()) << "already checked today -> not due again";

    settings.setUpdateLastCheckDate(QDate::currentDate().addDays(-1).toString(Qt::ISODate));
    EXPECT_TRUE(window.updateCheckIsDue()) << "yesterday's check does not cover today";

    // The toggle in the Config menu has to actually stop the checking.
    settings.setAutoUpdateCheck(false);
    EXPECT_FALSE(window.updateCheckIsDue());
}

// The window keeps asking for as long as it is open, so a check is not
// something that can only ever happen during startup.
TEST(ScheduledUpdateCheckTest, KeepsAskingWhileTheWindowStaysOpen) {
    MainWindow window;

    auto *timer = window.findChild<QTimer *>(QStringLiteral("ScheduledUpdateCheck"));
    ASSERT_NE(timer, nullptr) << "nothing re-checks after startup";
    EXPECT_TRUE(timer->isActive());
    EXPECT_EQ(timer->interval(), MainWindow::kUpdateCheckTickMs);
    // An hour is short enough that a day boundary is noticed promptly and long
    // enough that it costs nothing; a tick as long as the policy itself would
    // make the first re-check land a day late.
    EXPECT_LT(MainWindow::kUpdateCheckTickMs, 24 * 60 * 60 * 1000);
}
