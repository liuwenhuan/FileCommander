#include <gtest/gtest.h>

#include <QApplication>
#include <QDate>
#include <QLabel>
#include <QLineEdit>
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

// The user-facing half of the update check: the badge that says a release
// exists, and the dialog that says where to get it. Nothing here downloads or
// installs anything -- updates arrive through the Microsoft Store or by the
// user fetching the package themselves -- so what these assert is that the
// dialog hands over everything needed to do that by hand.
namespace {

QPushButton *buttonNamed(QWidget &parent, const QString &name) {
    return parent.findChild<QPushButton *>(name);
}

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
}

TEST(UpdateDialogTest, ReleaseWithoutNotesStillSaysSomething) {
    UpdateInfo info = sampleRelease();
    info.notes.clear();
    UpdateDialog dialog(info);

    auto *notes = dialog.findChild<QTextEdit *>();
    ASSERT_NE(notes, nullptr);
    EXPECT_FALSE(notes->toPlainText().isEmpty()) << "an empty notes box looks like a broken dialog";
}

// Fetching the package is the user's job now, so the address has to be
// available to copy -- not just behind a button that launches a browser, which
// is not always what somebody wants or is even able to do.
TEST(UpdateDialogTest, OffersTheDownloadAddressAsSelectableText) {
    const UpdateInfo info = sampleRelease();
    UpdateDialog dialog(info);

    EXPECT_EQ(dialog.downloadUrlText(), info.url);
    auto *field = dialog.findChild<QLineEdit *>(QStringLiteral("UpdateDownloadUrl"));
    ASSERT_NE(field, nullptr);
    EXPECT_TRUE(field->isReadOnly()) << "the address must not be editable";
    EXPECT_TRUE(field->isEnabled()) << "a disabled field cannot be selected or copied";

    ASSERT_NE(buttonNamed(dialog, QStringLiteral("UpdateDownloadButton")), nullptr);
    EXPECT_TRUE(buttonNamed(dialog, QStringLiteral("UpdateDownloadButton"))->isEnabled());
}

// Verification used to happen inside the installer. With the download handed
// back to the user, publishing the checksum is the only way they can make the
// same check the application used to make for them.
TEST(UpdateDialogTest, PublishesTheChecksumSoAManualDownloadCanBeVerified) {
    const UpdateInfo info = sampleRelease();
    UpdateDialog dialog(info);

    EXPECT_EQ(dialog.checksumText(), info.sha256);
    auto *field = dialog.findChild<QLineEdit *>(QStringLiteral("UpdateChecksum"));
    ASSERT_NE(field, nullptr);
    EXPECT_TRUE(field->isReadOnly());
    EXPECT_TRUE(field->isEnabled());
}

TEST(UpdateDialogTest, ShowsTheStoreOnlyWhenTheManifestNamesOne) {
    UpdateInfo withoutStore = sampleRelease();
    UpdateDialog plain(withoutStore);
    EXPECT_FALSE(plain.hasStoreButton())
        << "offering a store the manifest never mentioned would be a dead end";

    UpdateInfo withStore = sampleRelease();
    withStore.storeUrl = QStringLiteral("https://apps.microsoft.com/detail/example");
    UpdateDialog store(withStore);
    EXPECT_TRUE(store.hasStoreButton());
    ASSERT_NE(buttonNamed(store, QStringLiteral("UpdateStoreButton")), nullptr);
}

TEST(UpdateDialogTest, ClosingDismissesIt) {
    UpdateDialog dialog(sampleRelease());
    QPushButton *close = buttonWithText(dialog, QStringLiteral("Close"));
    ASSERT_NE(close, nullptr);

    close->click();

    EXPECT_FALSE(dialog.isVisible());
    EXPECT_EQ(dialog.result(), int(QDialog::Rejected));
}

// A manifest whose package URL was rejected still announces the release; the
// button that would go nowhere is what gets disabled.
TEST(UpdateDialogTest, AReleaseWithNoUsableUrlStillAnnouncesItself) {
    UpdateInfo info = sampleRelease();
    info.url.clear();
    UpdateDialog dialog(info);

    QPushButton *download = buttonNamed(dialog, QStringLiteral("UpdateDownloadButton"));
    ASSERT_NE(download, nullptr);
    EXPECT_FALSE(download->isEnabled());
    EXPECT_TRUE(dialog.downloadUrlText().isEmpty());
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
