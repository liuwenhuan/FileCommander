#include <gtest/gtest.h>

#include "MockHttpServer.h"
#include "TryUntil.h"
#include "account/AccountClient.h"
#include "config/Settings.h"
#include "dialogs/AccountDialog.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QStackedWidget>

// The dialog is glue, so these tests are about the glue: that a sign-in flips
// the dialog to the device page and lands in Settings, that a refused sign-in
// shows the server's own words and stays on the form, and that "create account"
// does not then make the user sign in by hand.
namespace {

constexpr int kTimeoutMs = 5000;

MockHttpServer::Route json(const QByteArray &body, int status = 200) {
    MockHttpServer::Route route;
    route.status = status;
    route.body = body;
    return route;
}

// Serves the two calls a successful sign-in makes: the login itself and the
// device list the dialog asks for immediately afterwards.
void serveSignIn(MockHttpServer &server) {
    server.setRoute("/v1/auth/login", json(R"({"access_token":"access-1",
                                               "refresh_token":"refresh-1",
                                               "device_id":"device-1"})"));
    server.setRoute("/v1/devices", json(R"([
        {"id":"device-1","name":"this box","platform":"linux","online":true,"self":true},
        {"id":"device-2","name":"laptop","platform":"linux","online":false,"self":false}])"));
}

template <typename T> T *child(AccountDialog &dialog, int index = 0) {
    const QList<T *> found = dialog.findChildren<T *>();
    return index < found.size() ? found.at(index) : nullptr;
}

int currentPage(AccountDialog &dialog) {
    auto *pages = child<QStackedWidget>(dialog);
    return pages ? pages->currentIndex() : -1;
}

QPushButton *button(AccountDialog &dialog, const QString &text) {
    for (QPushButton *b : dialog.findChildren<QPushButton *>()) {
        if (b->text() == text)
            return b;
    }
    return nullptr;
}

// Fills in the form the way a user would, against the mock server.
void fillForm(AccountDialog &dialog, MockHttpServer &server) {
    auto *custom = dialog.findChild<QRadioButton *>(QStringLiteral("CustomServerRadio"));
    auto *serverUrl = dialog.findChild<QLineEdit *>(QStringLiteral("CustomServerUrl"));
    auto *email = dialog.findChild<QLineEdit *>(QStringLiteral("AccountEmail"));
    auto *password = dialog.findChild<QLineEdit *>(QStringLiteral("AccountPassword"));
    auto *device = dialog.findChild<QLineEdit *>(QStringLiteral("AccountDeviceName"));
    ASSERT_NE(custom, nullptr);
    ASSERT_NE(serverUrl, nullptr);
    ASSERT_NE(email, nullptr);
    ASSERT_NE(password, nullptr);
    ASSERT_NE(device, nullptr);
    custom->click();
    serverUrl->setText(server.url(QString()));
    email->setText(QStringLiteral("someone@example.com"));
    password->setText(QStringLiteral("hunter2"));
    device->setText(QStringLiteral("this box"));
}

// Settings is process-wide storage (redirected to a test location by
// test_main), so the account keys are put back afterwards -- otherwise the
// first of these tests decides what the next one starts from.
class AccountSettingsGuard {
public:
    ~AccountSettingsGuard() {
        Settings settings;
        settings.setAccountEmail(QString());
        settings.setAccountDeviceId(QString());
        settings.setAccountUsesOfficialServer(true);
        settings.setAccountCustomServerUrl(QString());
        settings.setRememberAccountAutoLogin(true);
        settings.setAccountDeviceName(QString());
    }
};

} // namespace

TEST(AccountDialog, OfficialServerIsHostnameFreeAndAutomaticLoginStartsChecked) {
    AccountSettingsGuard guard;
    Settings settings;
    settings.setAccountUsesOfficialServer(true);
    settings.setAccountCustomServerUrl(QString());
    settings.setRememberAccountAutoLogin(true);

    AccountClient client;
    AccountDialog dialog(client, settings);
    auto *official = dialog.findChild<QRadioButton *>(QStringLiteral("OfficialServerRadio"));
    auto *customUrl = dialog.findChild<QLineEdit *>(QStringLiteral("CustomServerUrl"));
    auto *remember = dialog.findChild<QCheckBox *>(QStringLiteral("RememberAutoLogin"));
    ASSERT_NE(official, nullptr);
    ASSERT_NE(customUrl, nullptr);
    ASSERT_NE(remember, nullptr);
    EXPECT_TRUE(official->isChecked());
    EXPECT_TRUE(customUrl->isHidden());
    EXPECT_TRUE(remember->isChecked());

    for (QLabel *label : dialog.findChildren<QLabel *>()) {
        EXPECT_FALSE(label->text().contains(QStringLiteral("fm.aigutta.com")));
        EXPECT_FALSE(label->text().contains(QStringLiteral("sgvps.aigutta.com")));
    }
    EXPECT_FALSE(customUrl->placeholderText().contains(QStringLiteral("aigutta.com")));
}

TEST(AccountDialog, InvalidCustomServerDoesNotStartARequest) {
    AccountSettingsGuard guard;
    Settings settings;
    AccountClient client;
    AccountDialog dialog(client, settings);
    auto *custom = dialog.findChild<QRadioButton *>(QStringLiteral("CustomServerRadio"));
    auto *customUrl = dialog.findChild<QLineEdit *>(QStringLiteral("CustomServerUrl"));
    auto *status = dialog.findChild<QLabel *>(QStringLiteral("AccountStatus"));
    ASSERT_NE(custom, nullptr);
    ASSERT_NE(customUrl, nullptr);
    ASSERT_NE(status, nullptr);
    custom->click();
    customUrl->setText(QStringLiteral("ftp://example.com?token=bad"));

    button(dialog, QObject::tr("Sign In"))->click();

    EXPECT_EQ(status->text(),
              QCoreApplication::translate("AccountDialog", "Enter a valid server URL."));
    EXPECT_FALSE(client.isLoggedIn());
    EXPECT_TRUE(settings.accountUsesOfficialServer());
}

TEST(AccountDialog, UncheckedAutomaticLoginIsRememberedAfterSuccessfulSignIn) {
    AccountSettingsGuard guard;
    MockHttpServer server;
    ASSERT_NE(server.port(), 0);
    serveSignIn(server);

    Settings settings;
    AccountClient client;
    AccountDialog dialog(client, settings);
    ASSERT_NO_FATAL_FAILURE(fillForm(dialog, server));
    auto *remember = dialog.findChild<QCheckBox *>(QStringLiteral("RememberAutoLogin"));
    ASSERT_NE(remember, nullptr);
    remember->setChecked(false);

    button(dialog, QObject::tr("Sign In"))->click();

    FC_TRY_COMPARE_WITH_TIMEOUT(currentPage(dialog), 1, kTimeoutMs);
    EXPECT_FALSE(settings.rememberAccountAutoLogin());
}

TEST(AccountDialog, SigningInShowsTheDevicesAndRemembersTheAccount) {
    AccountSettingsGuard guard;
    MockHttpServer server;
    ASSERT_NE(server.port(), 0);
    serveSignIn(server);

    Settings settings;
    AccountClient client;
    AccountDialog dialog(client, settings);
    ASSERT_NO_FATAL_FAILURE(fillForm(dialog, server));
    ASSERT_EQ(currentPage(dialog), 0);

    button(dialog, QObject::tr("Sign In"))->click();

    FC_TRY_COMPARE_WITH_TIMEOUT(currentPage(dialog), 1, kTimeoutMs);
    auto *devices = child<QListWidget>(dialog);
    FC_TRY_COMPARE_WITH_TIMEOUT(devices->count(), 2, kTimeoutMs);
    EXPECT_TRUE(devices->item(0)->text().contains(QStringLiteral("this box")));
    EXPECT_TRUE(devices->item(1)->text().contains(QStringLiteral("laptop")));
    EXPECT_EQ(settings.accountEmail(), QStringLiteral("someone@example.com"));
    EXPECT_EQ(settings.accountDeviceId(), QStringLiteral("device-1"));
    // The device name is remembered so the next sign-in offers it, not the
    // hostname.
    EXPECT_EQ(settings.accountDeviceName(), QStringLiteral("this box"));
    // The custom endpoint is kept so a self-hosted server is entered once.
    EXPECT_FALSE(settings.accountUsesOfficialServer());
    EXPECT_EQ(settings.accountCustomServerUrl(), server.url(QString()));
    EXPECT_TRUE(settings.rememberAccountAutoLogin());
}

TEST(AccountDialog, ARefusedSignInStaysOnTheFormAndShowsTheServersReason) {
    AccountSettingsGuard guard;
    MockHttpServer server;
    ASSERT_NE(server.port(), 0);
    server.setRoute("/v1/auth/login", json(R"({"detail":"Incorrect email or password"})", 401));

    Settings settings;
    AccountClient client;
    AccountDialog dialog(client, settings);
    ASSERT_NO_FATAL_FAILURE(fillForm(dialog, server));

    QPushButton *signIn = button(dialog, QObject::tr("Sign In"));
    signIn->click();

    auto *status = dialog.findChildren<QLabel *>().last();
    FC_TRY_VERIFY_WITH_TIMEOUT(
        status->text().contains(QStringLiteral("Incorrect email or password")), kTimeoutMs);
    EXPECT_EQ(currentPage(dialog), 0);
    // A failure has to re-enable the button, or the user gets one attempt.
    EXPECT_TRUE(signIn->isEnabled());
    EXPECT_TRUE(settings.accountDeviceId().isEmpty());
    EXPECT_FALSE(settings.accountUsesOfficialServer());
    EXPECT_EQ(settings.accountCustomServerUrl(), server.url(QString()));
    EXPECT_FALSE(settings.rememberAccountAutoLogin());
}

TEST(AccountDialog, TheDeviceNamePrefillsFromLastSignInNotTheHostname) {
    AccountSettingsGuard guard;
    Settings settings;
    settings.setAccountDeviceName(QStringLiteral("my tower"));

    AccountClient client;
    AccountDialog dialog(client, settings);

    auto *device = dialog.findChild<QLineEdit *>(QStringLiteral("AccountDeviceName"));
    ASSERT_NE(device, nullptr);
    EXPECT_EQ(device->text(), QStringLiteral("my tower"));
}

TEST(AccountDialog, CreatingAnAccountSignsInWithoutAskingAgain) {
    AccountSettingsGuard guard;
    MockHttpServer server;
    ASSERT_NE(server.port(), 0);
    serveSignIn(server);
    server.setRoute("/v1/auth/register", json(R"({"email":"someone@example.com"})", 201));

    Settings settings;
    AccountClient client;
    AccountDialog dialog(client, settings);
    ASSERT_NO_FATAL_FAILURE(fillForm(dialog, server));

    button(dialog, QObject::tr("Create Account"))->click();

    FC_TRY_COMPARE_WITH_TIMEOUT(currentPage(dialog), 1, kTimeoutMs);
    EXPECT_EQ(server.requestCount(QStringLiteral("/v1/auth/register")), 1);
    EXPECT_EQ(server.requestCount(QStringLiteral("/v1/auth/login")), 1);
    EXPECT_FALSE(settings.accountUsesOfficialServer());
    EXPECT_EQ(settings.accountCustomServerUrl(), server.url(QString()));
    EXPECT_TRUE(settings.rememberAccountAutoLogin());
}
