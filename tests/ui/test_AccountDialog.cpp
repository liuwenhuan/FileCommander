#include <gtest/gtest.h>

#include "MockHttpServer.h"
#include "TryUntil.h"
#include "account/AccountClient.h"
#include "config/Settings.h"
#include "dialogs/AccountDialog.h"
#include "ThemeStateGuard.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFile>
#include <QFont>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTranslator>
#include <QWidget>

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
        settings.setAccountDeviceName(QString());
    }
};

} // namespace

TEST(AccountDialog, OfficialServerIsHostnameFree) {
    AccountSettingsGuard guard;
    Settings settings;
    settings.setAccountUsesOfficialServer(true);
    settings.setAccountCustomServerUrl(QString());

    AccountClient client;
    AccountDialog dialog(client, settings);
    auto *official = dialog.findChild<QRadioButton *>(QStringLiteral("OfficialServerRadio"));
    auto *customUrl = dialog.findChild<QLineEdit *>(QStringLiteral("CustomServerUrl"));
    ASSERT_NE(official, nullptr);
    ASSERT_NE(customUrl, nullptr);
    EXPECT_TRUE(official->isChecked());
    EXPECT_TRUE(customUrl->isHidden());

    for (QLabel *label : dialog.findChildren<QLabel *>()) {
        EXPECT_FALSE(label->text().contains(QStringLiteral("fc.aigutta.com")));
        EXPECT_FALSE(label->text().contains(QStringLiteral("fm.aigutta.com")));
        EXPECT_FALSE(label->text().contains(QStringLiteral("sgvps.aigutta.com")));
    }
    EXPECT_FALSE(customUrl->placeholderText().contains(QStringLiteral("aigutta.com")));
}

TEST(AccountDialog, SignedOutFormIsCenteredWithoutARedundantCloseButton) {
    AccountSettingsGuard guard;
    Settings settings;
    AccountClient client;
    AccountDialog dialog(client, settings);
    dialog.show();
    QCoreApplication::processEvents();

    auto *official = dialog.findChild<QRadioButton *>(QStringLiteral("OfficialServerRadio"));
    auto *status = dialog.findChild<QLabel *>(QStringLiteral("AccountStatus"));
    QPushButton *signIn = button(dialog, QObject::tr("Sign In"));
    ASSERT_NE(official, nullptr);
    ASSERT_NE(status, nullptr);
    ASSERT_NE(signIn, nullptr);
    EXPECT_EQ(dialog.size(), QSize(500, 350));
    EXPECT_EQ(button(dialog, QObject::tr("Close")), nullptr);

    const int formTop = official->mapTo(&dialog, QPoint(0, 0)).y();
    const int actionsBottom = signIn->mapTo(&dialog, QPoint(0, signIn->height())).y();
    const QRect content = dialog.contentsRect();
    EXPECT_LE(qAbs((formTop - content.top()) - (content.bottom() - actionsBottom)), 48);
    EXPECT_TRUE(status->isHidden());
}

TEST(AccountDialog, ServerRowsStayAlignedInDarkTheme) {
    ThemeStateGuard themeState;
    QFile theme(QStringLiteral(":/themes/dark.qss"));
    ASSERT_TRUE(theme.open(QIODevice::ReadOnly | QIODevice::Text));
    qApp->setStyleSheet(QString::fromUtf8(theme.readAll()));
    QFont font = qApp->font();
    font.setPointSize(13);
    qApp->setFont(font);

    AccountSettingsGuard guard;
    Settings settings;
    settings.setAccountUsesOfficialServer(true);
    AccountClient client;
    AccountDialog dialog(client, settings);
    auto *official = dialog.findChild<QRadioButton *>(QStringLiteral("OfficialServerRadio"));
    auto *custom = dialog.findChild<QRadioButton *>(QStringLiteral("CustomServerRadio"));
    auto *url = dialog.findChild<QLineEdit *>(QStringLiteral("CustomServerUrl"));
    auto *scroll = dialog.findChild<QScrollArea *>(QStringLiteral("AccountLoginScroll"));
    ASSERT_NE(official, nullptr);
    ASSERT_NE(custom, nullptr);
    ASSERT_NE(url, nullptr);
    ASSERT_NE(scroll, nullptr);
    dialog.show();
    QCoreApplication::processEvents();

    EXPECT_TRUE(url->isHidden());
    const int officialTop = official->mapTo(&dialog, QPoint(0, 0)).y();
    const int customTop = custom->mapTo(&dialog, QPoint(0, 0)).y();
    EXPECT_GT(customTop, officialTop);
    EXPECT_LE(customTop - officialTop, official->sizeHint().height() + 16);
    EXPECT_LE(qAbs(custom->mapTo(&dialog, QPoint(0, 0)).x()
                       - official->mapTo(&dialog, QPoint(0, 0)).x()), 2);
    custom->click();
    QCoreApplication::processEvents();
    EXPECT_TRUE(custom->isChecked());
    EXPECT_FALSE(official->isChecked());
    EXPECT_TRUE(url->isVisible());
    const int urlLeft = url->mapTo(&dialog, QPoint(0, 0)).x();
    const int customRight = custom->mapTo(&dialog, QPoint(custom->sizeHint().width(), 0)).x();
    const int urlRight = url->mapTo(&dialog, QPoint(url->width(), 0)).x();
    const int viewportRight = scroll->viewport()->mapTo(
        &dialog, QPoint(scroll->viewport()->width(), 0)).x();
    EXPECT_GE(urlLeft - customRight, 0);
    EXPECT_LE(urlLeft - customRight, 8);
    EXPECT_LE(qAbs((viewportRight - 16) - urlRight), 2);
}

TEST(AccountDialog, CustomServerFieldKeepsTheCompactLoginSize) {
    AccountSettingsGuard guard;
    Settings settings;
    AccountClient client;
    AccountDialog dialog(client, settings);
    auto *custom = dialog.findChild<QRadioButton *>(QStringLiteral("CustomServerRadio"));
    auto *url = dialog.findChild<QLineEdit *>(QStringLiteral("CustomServerUrl"));
    auto *email = dialog.findChild<QLineEdit *>(QStringLiteral("AccountEmail"));
    ASSERT_NE(custom, nullptr);
    ASSERT_NE(url, nullptr);
    ASSERT_NE(email, nullptr);
    dialog.show();
    custom->click();
    QCoreApplication::processEvents();

    EXPECT_EQ(dialog.size(), QSize(500, 350));
    EXPECT_TRUE(url->isVisible());
    auto *official = dialog.findChild<QRadioButton *>(QStringLiteral("OfficialServerRadio"));
    ASSERT_NE(official, nullptr);
    EXPECT_TRUE(custom->isChecked());
    EXPECT_FALSE(official->isChecked());
    EXPECT_GT(custom->mapTo(&dialog, QPoint(0, 0)).y(),
              official->mapTo(&dialog, QPoint(0, 0)).y());
    const int customRight = custom->mapTo(&dialog, QPoint(custom->sizeHint().width(), 0)).x();
    const int urlLeft = url->mapTo(&dialog, QPoint(0, 0)).x();
    EXPECT_GE(urlLeft - customRight, 0);
    EXPECT_LE(urlLeft - customRight, 8);
    const int urlBottom = url->mapTo(&dialog, QPoint(0, url->height())).y();
    const int emailTop = email->mapTo(&dialog, QPoint(0, 0)).y();
    EXPECT_LT(urlBottom, emailTop);
}

TEST(AccountDialog, LargerFontKeepsCustomServerAndStatusVisible) {
    AccountSettingsGuard guard;
    Settings settings;
    AccountClient client;
    AccountDialog dialog(client, settings);
    QFont largeFont = dialog.font();
    largeFont.setPointSize(14);
    dialog.setFont(largeFont);
    auto *custom = dialog.findChild<QRadioButton *>(QStringLiteral("CustomServerRadio"));
    auto *url = dialog.findChild<QLineEdit *>(QStringLiteral("CustomServerUrl"));
    auto *status = dialog.findChild<QLabel *>(QStringLiteral("AccountStatus"));
    ASSERT_NE(custom, nullptr);
    ASSERT_NE(url, nullptr);
    ASSERT_NE(status, nullptr);
    dialog.show();
    custom->click();
    QCoreApplication::processEvents();
    EXPECT_GE(url->mapTo(&dialog, QPoint(0, 0)).y(), dialog.contentsRect().top());
    ASSERT_TRUE(QMetaObject::invokeMethod(&client, "requestFailed", Qt::DirectConnection,
                                          Q_ARG(QString, QStringLiteral("The account server could not be reached. Please check the address and try again."))));
    QCoreApplication::processEvents();

    EXPECT_EQ(dialog.size(), QSize(500, 350));
    auto *scroll = dialog.findChild<QScrollArea *>(QStringLiteral("AccountLoginScroll"));
    ASSERT_NE(scroll, nullptr);
    EXPECT_LE(status->mapTo(&dialog, QPoint(0, status->height() - 1)).y(),
              dialog.contentsRect().bottom());
}

TEST(AccountDialog, InvalidCustomServerDoesNotStartARequest) {
    AccountSettingsGuard guard;
    Settings settings;
    AccountClient client;
    AccountDialog dialog(client, settings);
    auto *custom = dialog.findChild<QRadioButton *>(QStringLiteral("CustomServerRadio"));
    auto *customUrl = dialog.findChild<QLineEdit *>(QStringLiteral("CustomServerUrl"));
    auto *email = dialog.findChild<QLineEdit *>(QStringLiteral("AccountEmail"));
    auto *status = dialog.findChild<QLabel *>(QStringLiteral("AccountStatus"));
    ASSERT_NE(custom, nullptr);
    ASSERT_NE(customUrl, nullptr);
    ASSERT_NE(email, nullptr);
    ASSERT_NE(status, nullptr);
    custom->click();
    customUrl->setText(QStringLiteral("ftp://example.com?token=bad"));
    email->setText(QStringLiteral("valid@example.com"));

    button(dialog, QObject::tr("Sign In"))->click();

    EXPECT_EQ(status->text(),
              QCoreApplication::translate("AccountDialog", "Enter a valid server URL."));
    EXPECT_FALSE(client.isLoggedIn());
    EXPECT_TRUE(settings.accountUsesOfficialServer());
}

TEST(AccountDialog, InvalidEmailDoesNotSubmitEitherAuthRequest) {
    AccountSettingsGuard guard;
    MockHttpServer server;
    ASSERT_NE(server.port(), 0);
    Settings settings;
    AccountClient client;
    AccountDialog dialog(client, settings);
    ASSERT_NO_FATAL_FAILURE(fillForm(dialog, server));
    auto *email = dialog.findChild<QLineEdit *>(QStringLiteral("AccountEmail"));
    auto *status = dialog.findChild<QLabel *>(QStringLiteral("AccountStatus"));
    QPushButton *signIn = button(dialog, QObject::tr("Sign In"));
    QPushButton *create = button(dialog, QObject::tr("Create Account"));
    ASSERT_NE(email, nullptr);
    ASSERT_NE(status, nullptr);
    ASSERT_NE(signIn, nullptr);
    ASSERT_NE(create, nullptr);
    email->setText(QStringLiteral("not-an-email"));
    dialog.show();
    QCoreApplication::processEvents();

    signIn->click();
    EXPECT_EQ(status->text(), QCoreApplication::translate("AccountDialog", "Enter a valid email address."));
    EXPECT_TRUE(email->hasFocus());
    EXPECT_TRUE(signIn->isEnabled());
    EXPECT_TRUE(create->isEnabled());
    EXPECT_EQ(server.requestCount(QStringLiteral("/v1/auth/login")), 0);
    EXPECT_EQ(server.requestCount(QStringLiteral("/v1/auth/register")), 0);
    EXPECT_TRUE(settings.accountUsesOfficialServer());
    EXPECT_TRUE(settings.accountCustomServerUrl().isEmpty());

    create->click();
    EXPECT_EQ(status->text(), QCoreApplication::translate("AccountDialog", "Enter a valid email address."));
    EXPECT_TRUE(email->hasFocus());
    EXPECT_TRUE(signIn->isEnabled());
    EXPECT_TRUE(create->isEnabled());
    EXPECT_EQ(server.requestCount(QStringLiteral("/v1/auth/login")), 0);
    EXPECT_EQ(server.requestCount(QStringLiteral("/v1/auth/register")), 0);
}

TEST(AccountDialog, CanonicalizesEmailBeforeSignInAndPersistence) {
    AccountSettingsGuard guard;
    MockHttpServer server;
    ASSERT_NE(server.port(), 0);
    serveSignIn(server);

    Settings settings;
    AccountClient client;
    AccountDialog dialog(client, settings);
    ASSERT_NO_FATAL_FAILURE(fillForm(dialog, server));
    auto *email = dialog.findChild<QLineEdit *>(QStringLiteral("AccountEmail"));
    ASSERT_NE(email, nullptr);
    email->setText(QStringLiteral(" User.Name+tag@EXAMPLE.com "));

    button(dialog, QObject::tr("Sign In"))->click();

    FC_TRY_COMPARE_WITH_TIMEOUT(currentPage(dialog), 1, kTimeoutMs);
    EXPECT_EQ(email->text(), QStringLiteral("user.name+tag@example.com"));
    EXPECT_EQ(settings.accountEmail(), QStringLiteral("user.name+tag@example.com"));
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
}

TEST(AccountDialog, DialogExpandsForDevicesAndShrinksAfterSignOut) {
    AccountSettingsGuard guard;
    MockHttpServer server;
    ASSERT_NE(server.port(), 0);
    serveSignIn(server);
    Settings settings;
    AccountClient client;
    AccountDialog dialog(client, settings);
    ASSERT_NO_FATAL_FAILURE(fillForm(dialog, server));
    dialog.show();
    EXPECT_EQ(dialog.size(), QSize(500, 350));
    const QPoint windowCenter = dialog.geometry().center();
    button(dialog, QObject::tr("Sign In"))->click();
    FC_TRY_COMPARE_WITH_TIMEOUT(currentPage(dialog), 1, kTimeoutMs);
    QCoreApplication::processEvents();

    auto *pages = child<QStackedWidget>(dialog);
    QPushButton *signOut = button(dialog, QObject::tr("Sign Out"));
    auto *devices = child<QListWidget>(dialog);
    ASSERT_NE(pages, nullptr);
    ASSERT_NE(signOut, nullptr);
    ASSERT_NE(devices, nullptr);
    EXPECT_EQ(dialog.size(), QSize(500, 430));
    EXPECT_LE((dialog.geometry().center() - windowCenter).manhattanLength(), 4);
    EXPECT_GE(devices->height(), 52);
    auto *sharedFolders = dialog.findChildren<QListWidget *>().value(1);
    ASSERT_NE(sharedFolders, nullptr);
    EXPECT_GE(sharedFolders->height(), 52);
    EXPECT_LE(signOut->mapTo(&dialog, QPoint(0, signOut->height())).y(),
              dialog.contentsRect().bottom());
    auto *status = dialog.findChild<QLabel *>(QStringLiteral("AccountSignedInStatus"));
    ASSERT_NE(status, nullptr);
    EXPECT_FALSE(status->isVisible());
    ASSERT_TRUE(QMetaObject::invokeMethod(&client, "requestFailed", Qt::DirectConnection,
                                          Q_ARG(QString, QStringLiteral("Device request failed"))));
    EXPECT_TRUE(status->isVisible());
    EXPECT_EQ(status->text(), QStringLiteral("Device request failed"));
    EXPECT_LE(status->mapTo(&dialog, QPoint(0, status->height())).y(),
              dialog.contentsRect().bottom());

    signOut->click();
    EXPECT_EQ(currentPage(dialog), 0);
    EXPECT_EQ(dialog.size(), QSize(500, 350));
    EXPECT_LE((dialog.geometry().center() - windowCenter).manhattanLength(), 4);
}

TEST(AccountDialog, ThemedModalDialogReturnsToItsLoginSizeAfterSignOut) {
    ThemeStateGuard themeState;
    QFile theme(QStringLiteral(":/themes/green.qss"));
    ASSERT_TRUE(theme.open(QIODevice::ReadOnly | QIODevice::Text));
    qApp->setStyleSheet(QString::fromUtf8(theme.readAll()));
    QFont font = qApp->font();
    font.setPointSize(13);
    qApp->setFont(font);

    AccountSettingsGuard guard;
    MockHttpServer server;
    ASSERT_NE(server.port(), 0);
    serveSignIn(server);
    Settings settings;
    AccountClient client;
    QWidget parent;
    parent.resize(900, 700);
    parent.show();
    AccountDialog dialog(client, settings, &parent);
    ASSERT_NO_FATAL_FAILURE(fillForm(dialog, server));
    dialog.open();
    QCoreApplication::processEvents();
    const QSize loginSize = dialog.size();
    EXPECT_EQ(loginSize, QSize(500, 350));
    auto *url = dialog.findChild<QLineEdit *>(QStringLiteral("CustomServerUrl"));
    auto *scroll = dialog.findChild<QScrollArea *>(QStringLiteral("AccountLoginScroll"));
    auto *email = dialog.findChild<QLineEdit *>(QStringLiteral("AccountEmail"));
    auto *password = dialog.findChild<QLineEdit *>(QStringLiteral("AccountPassword"));
    auto *device = dialog.findChild<QLineEdit *>(QStringLiteral("AccountDeviceName"));
    ASSERT_NE(url, nullptr);
    ASSERT_NE(scroll, nullptr);
    ASSERT_NE(email, nullptr);
    ASSERT_NE(password, nullptr);
    ASSERT_NE(device, nullptr);
    EXPECT_GE(url->height(), url->sizeHint().height());

    QPushButton *signIn = button(dialog, QObject::tr("Sign In"));
    QPushButton *signOut = button(dialog, QObject::tr("Sign Out"));
    QPushButton *registerButton = button(dialog, QObject::tr("Create Account"));
    auto *official = dialog.findChild<QRadioButton *>(QStringLiteral("OfficialServerRadio"));
    auto *custom = dialog.findChild<QRadioButton *>(QStringLiteral("CustomServerRadio"));
    ASSERT_NE(signIn, nullptr);
    ASSERT_NE(signOut, nullptr);
    ASSERT_NE(registerButton, nullptr);
    ASSERT_NE(official, nullptr);
    ASSERT_NE(custom, nullptr);
    EXPECT_EQ(scroll->verticalScrollBar()->maximum(), 0);
    const int viewportLeft = scroll->viewport()->mapTo(&dialog, QPoint(0, 0)).x();
    const int viewportRight = viewportLeft + scroll->viewport()->width();
    EXPECT_GE(official->mapTo(&dialog, QPoint(0, 0)).x() - viewportLeft, 16);
    EXPECT_GE(viewportRight - signIn->mapTo(&dialog, QPoint(signIn->width(), 0)).x(), 16);
    EXPECT_EQ(official->text(), QObject::tr("Official server"));
    EXPECT_EQ(custom->text(), QObject::tr("Custom server"));
    EXPECT_EQ(device->accessibleName(), QObject::tr("Device name"));
    auto *deviceLabel = dialog.findChild<QLabel *>(QStringLiteral("AccountDeviceLabel"));
    ASSERT_NE(deviceLabel, nullptr);
    EXPECT_EQ(deviceLabel->text(), QObject::tr("Device name"));
    EXPECT_GE(email->width(), 140);
    EXPECT_GE(password->width(), 140);
    EXPECT_EQ(email->placeholderText(), QObject::tr("Email:"));
    EXPECT_EQ(password->placeholderText(), QObject::tr("Password:"));
    EXPECT_GE(device->width(), 240);
    const int viewportTop = scroll->viewport()->mapTo(&dialog, QPoint(0, 0)).y();
    const int viewportBottom = scroll->viewport()->mapTo(
        &dialog, QPoint(0, scroll->viewport()->height())).y();
    const int serverTop = official->mapTo(&dialog, QPoint(0, 0)).y();
    const int actionsBottom = signIn->mapTo(&dialog, QPoint(0, signIn->height())).y();
    EXPECT_LE(serverTop - viewportTop, 18);
    EXPECT_LE(viewportBottom - actionsBottom, 18);
    for (QWidget *field : {static_cast<QWidget *>(url), static_cast<QWidget *>(email),
                           static_cast<QWidget *>(password), static_cast<QWidget *>(device),
                           static_cast<QWidget *>(signIn), static_cast<QWidget *>(registerButton),
                           static_cast<QWidget *>(official), static_cast<QWidget *>(custom)}) {
        SCOPED_TRACE(field->objectName().toStdString());
        EXPECT_GE(field->mapTo(&dialog, QPoint(0, 0)).y(), dialog.contentsRect().top());
        EXPECT_LE(field->mapTo(&dialog, QPoint(0, field->height())).y(),
                  dialog.contentsRect().bottom());
        EXPECT_GE(field->mapTo(&dialog, QPoint(0, 0)).x(), dialog.contentsRect().left());
        EXPECT_LE(field->mapTo(&dialog, QPoint(field->width(), 0)).x(),
                  dialog.contentsRect().right());
    }
    signIn->click();
    FC_TRY_COMPARE_WITH_TIMEOUT(currentPage(dialog), 1, kTimeoutMs);
    QCoreApplication::processEvents();
    EXPECT_EQ(dialog.size(), QSize(500, 430));

    signOut->click();
    QCoreApplication::processEvents();
    EXPECT_EQ(currentPage(dialog), 0);
    EXPECT_EQ(dialog.size(), loginSize);
    EXPECT_GE(url->height(), url->sizeHint().height());

    official->click();
    QCoreApplication::processEvents();
    EXPECT_EQ(dialog.size(), QSize(500, 350));
    EXPECT_TRUE(url->isHidden());
    EXPECT_GE(official->height(), official->sizeHint().height());
}

TEST(AccountDialog, LoginControlsFitAcrossBundledLanguages) {
    ThemeStateGuard themeState;
    QFile theme(QStringLiteral(":/themes/green.qss"));
    ASSERT_TRUE(theme.open(QIODevice::ReadOnly | QIODevice::Text));
    qApp->setStyleSheet(QString::fromUtf8(theme.readAll()));
    QFont font = qApp->font();
    font.setPointSize(13);
    qApp->setFont(font);

    AccountSettingsGuard guard;
    Settings settings;
    settings.setAccountUsesOfficialServer(false);
    settings.setAccountCustomServerUrl(QStringLiteral("https://example.test"));
    AccountClient client;
    for (const QString &language : {QStringLiteral("de"), QStringLiteral("es"),
                                    QStringLiteral("fr"), QStringLiteral("ja"),
                                    QStringLiteral("ko"), QStringLiteral("pt_BR"),
                                    QStringLiteral("ru"), QStringLiteral("zh_CN"),
                                    QStringLiteral("zh_TW")}) {
        SCOPED_TRACE(language.toStdString());
        QTranslator translator;
        if (!translator.load(QStringLiteral(":/translations/ttc_%1.qm").arg(language))) {
            ADD_FAILURE() << "Missing compiled catalog";
            continue;
        }
        qApp->installTranslator(&translator);
        {
            AccountDialog dialog(client, settings);
            dialog.show();
            QCoreApplication::processEvents();
            auto *scroll = dialog.findChild<QScrollArea *>(QStringLiteral("AccountLoginScroll"));
            auto *url = dialog.findChild<QLineEdit *>(QStringLiteral("CustomServerUrl"));
            auto *email = dialog.findChild<QLineEdit *>(QStringLiteral("AccountEmail"));
            auto *password = dialog.findChild<QLineEdit *>(QStringLiteral("AccountPassword"));
            auto *usernameLabel = dialog.findChild<QLabel *>(QStringLiteral("AccountUsernameLabel"));
            auto *passwordLabel = dialog.findChild<QLabel *>(QStringLiteral("AccountPasswordLabel"));
            auto *official = dialog.findChild<QRadioButton *>(QStringLiteral("OfficialServerRadio"));
            auto *custom = dialog.findChild<QRadioButton *>(QStringLiteral("CustomServerRadio"));
            auto *signIn = button(dialog, QCoreApplication::translate("AccountDialog", "Sign In"));
            auto *registerButton = button(dialog, QCoreApplication::translate("AccountDialog", "Create Account"));
            const QString geometry = scroll && url && official && custom
                ? QStringLiteral("viewport=%1x%2 form=%3x%4 min=%5x%6 official='%7'(%8) custom='%9'(%10) url=%11@%12 signIn=%13 register='%14'(%15)")
                      .arg(scroll->viewport()->width()).arg(scroll->viewport()->height())
                      .arg(scroll->widget()->width()).arg(scroll->widget()->height())
                      .arg(scroll->widget()->minimumSizeHint().width())
                      .arg(scroll->widget()->minimumSizeHint().height())
                      .arg(official->text()).arg(official->width())
                      .arg(custom->text()).arg(custom->width())
                      .arg(url->width()).arg(url->mapTo(&dialog, QPoint(0, 0)).x())
                      .arg(signIn ? signIn->width() : -1)
                      .arg(registerButton ? registerButton->text() : QString())
                      .arg(registerButton ? registerButton->width() : -1)
                : QStringLiteral("missing control");
            SCOPED_TRACE(geometry.toStdString());
            EXPECT_EQ(dialog.size(), QSize(500, 350));
            EXPECT_TRUE(scroll && scroll->verticalScrollBar()->maximum() == 0);
            EXPECT_TRUE(usernameLabel && passwordLabel && email && password);
            if (usernameLabel && passwordLabel && email && password) {
                EXPECT_NE(usernameLabel->text(), QStringLiteral("Username"));
                EXPECT_NE(passwordLabel->text(), QStringLiteral("Password:"));
                EXPECT_EQ(email->accessibleName(), usernameLabel->text());
                EXPECT_EQ(usernameLabel->mapTo(&dialog, QPoint(0, 0)).x(),
                          email->mapTo(&dialog, QPoint(0, 0)).x());
                EXPECT_EQ(passwordLabel->mapTo(&dialog, QPoint(0, 0)).x(),
                          password->mapTo(&dialog, QPoint(0, 0)).x());
                EXPECT_LE(usernameLabel->mapTo(&dialog, QPoint(0, usernameLabel->height())).y(),
                          email->mapTo(&dialog, QPoint(0, 0)).y());
                EXPECT_LE(passwordLabel->mapTo(&dialog, QPoint(0, passwordLabel->height())).y(),
                          password->mapTo(&dialog, QPoint(0, 0)).y());
            }
            if (scroll && official && signIn) {
                const int left = scroll->viewport()->mapTo(&dialog, QPoint(0, 0)).x();
                const int right = left + scroll->viewport()->width();
                EXPECT_GE(official->mapTo(&dialog, QPoint(0, 0)).x() - left, 16);
                EXPECT_GE(right - signIn->mapTo(&dialog, QPoint(signIn->width(), 0)).x(), 16);
            }
            EXPECT_TRUE(url && url->width() >= 100);
            if (url && scroll) {
                const int viewportRight = scroll->viewport()->mapTo(
                    &dialog, QPoint(scroll->viewport()->width(), 0)).x();
                const int urlRight = url->mapTo(&dialog, QPoint(url->width(), 0)).x();
                EXPECT_LE(qAbs((viewportRight - 16) - urlRight), 2);
            }
            if (url && custom) {
                const int customRight = custom->mapTo(&dialog, QPoint(custom->sizeHint().width(), 0)).x();
                const int urlLeft = url->mapTo(&dialog, QPoint(0, 0)).x();
                EXPECT_GE(urlLeft - customRight, 0);
                EXPECT_LE(urlLeft - customRight, 8);
            }
            if (url)
                EXPECT_LE(url->mapTo(&dialog, QPoint(url->width(), 0)).x(),
                          dialog.contentsRect().right());
            if (signIn)
                EXPECT_LE(signIn->mapTo(&dialog, QPoint(0, signIn->height())).y(),
                          dialog.contentsRect().bottom());
            if (registerButton)
                EXPECT_LE(registerButton->mapTo(&dialog, QPoint(registerButton->width(), 0)).x(),
                          dialog.contentsRect().right());
            if (language == QStringLiteral("zh_CN")) {
                EXPECT_EQ(official->text(), QStringLiteral("官方服务器"));
                EXPECT_EQ(custom->text(), QStringLiteral("自定义服务器"));
                if (usernameLabel)
                    EXPECT_EQ(usernameLabel->text(), QStringLiteral("用户名"));
                if (passwordLabel)
                    EXPECT_EQ(passwordLabel->text(), QStringLiteral("密码："));
                auto *device = dialog.findChild<QLineEdit *>(QStringLiteral("AccountDeviceName"));
                auto *deviceLabel = dialog.findChild<QLabel *>(QStringLiteral("AccountDeviceLabel"));
                EXPECT_TRUE(device != nullptr && deviceLabel != nullptr);
                if (device)
                    EXPECT_EQ(device->accessibleName(), QStringLiteral("设备名称"));
                if (deviceLabel)
                    EXPECT_EQ(deviceLabel->text(), QStringLiteral("设备名称"));
            }
        }
        qApp->removeTranslator(&translator);
    }
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

    auto *status = dialog.findChild<QLabel *>(QStringLiteral("AccountStatus"));
    ASSERT_NE(status, nullptr);
    FC_TRY_VERIFY_WITH_TIMEOUT(
        status->text().contains(QStringLiteral("Incorrect email or password")), kTimeoutMs);
    EXPECT_EQ(currentPage(dialog), 0);
    // A failure has to re-enable the button, or the user gets one attempt.
    EXPECT_TRUE(signIn->isEnabled());
    EXPECT_TRUE(settings.accountDeviceId().isEmpty());
    EXPECT_FALSE(settings.accountUsesOfficialServer());
    EXPECT_EQ(settings.accountCustomServerUrl(), server.url(QString()));
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
}
