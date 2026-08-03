#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QSignalSpy>

#include <memory>

#include "FileProvider.h"
#include "NetworkSession.h"

#ifdef Q_OS_WIN
#include "WindowsSmbProvider.h"
#include "WindowsSmbSession.h"

#include <Windows.h>
#include <Winnetwk.h>
#endif

// An empty listing used to be the only thing a provider could say, so "the
// server refused to enumerate its shares" and "this folder is empty" arrived at
// the panel as the same answer. These pin the channel that separates them and
// the recovery each one gets.

namespace {

// A provider whose list() outcome the test dictates.
class ListingProvider : public FileProvider {
public:
    QVector<FileInfo> list(const QString &, bool) const override { return {}; }
    bool isDir(const QString &) const override { return true; }
    QString cleanPath(const QString &p) const override { return p; }
    QString parentPath(const QString &) const override { return {}; }
    bool exists(const QString &) const override { return true; }
    RenameResult rename(const QString &, const QString &, QString *) override {
        return RenameResult::Failed;
    }

    ListStatus lastListStatus() const override { return m_status; }
    QString lastListError() const override { return m_error; }
    void setListOutcome(ListStatus status, const QString &error) {
        m_status = status;
        m_error = error;
    }

private:
    ListStatus m_status = ListStatus::Ok;
    QString m_error;
};

template <typename Pred>
bool spinUntil(Pred pred, int budgetMs) {
    QElapsedTimer t;
    t.start();
    while (!pred() && t.elapsed() < budgetMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return pred();
}

// A connected session over `provider`, ready to take list requests.
std::shared_ptr<NetworkSession> connectedSession(std::shared_ptr<ListingProvider> provider) {
    std::shared_ptr<NetworkSession> session(new NetworkSession(provider),
                                            [](NetworkSession *s) { s->shutdownAsync(); });
    session->start([](QString *) { return true; }, QStringLiteral("/"));
    spinUntil([&] { return session->state() == NetworkSession::Connected; }, 5000);
    return session;
}

} // namespace

TEST(SmbListingErrors, ADeniedListingAsksForCredentialsInsteadOfLookingEmpty) {
    auto provider = std::make_shared<ListingProvider>();
    auto session = connectedSession(provider);
    ASSERT_EQ(session->state(), NetworkSession::Connected);

    QSignalSpy authSpy(session.get(), &NetworkSession::authRequired);
    QSignalSpy failedSpy(session.get(), &NetworkSession::listFailed);
    QSignalSpy readySpy(session.get(), &NetworkSession::listReady);

    provider->setListOutcome(FileProvider::ListStatus::AccessDenied,
                             QStringLiteral("Access is denied."));
    session->requestList(1, QStringLiteral("/"), false);
    ASSERT_TRUE(spinUntil([&] { return failedSpy.size() > 0; }, 5000));

    // The whole point: a denial is answerable, so the user is asked rather than
    // shown a blank pane. listReady would have been the old behaviour -- an
    // empty vector reported as a successful listing.
    EXPECT_EQ(readySpy.size(), 0);
    ASSERT_EQ(authSpy.size(), 1);
    EXPECT_EQ(authSpy.at(0).at(0).toString(), QStringLiteral("Access is denied."));
    // And the reason travels with the failure so the panel can name it.
    ASSERT_EQ(failedSpy.size(), 1);
    EXPECT_EQ(failedSpy.at(0).at(2).toString(), QStringLiteral("Access is denied."));
}

TEST(SmbListingErrors, RepeatedDeniedListingsRaiseOnlyOnePrompt) {
    auto provider = std::make_shared<ListingProvider>();
    auto session = connectedSession(provider);
    QSignalSpy authSpy(session.get(), &NetworkSession::authRequired);
    QSignalSpy failedSpy(session.get(), &NetworkSession::listFailed);

    provider->setListOutcome(FileProvider::ListStatus::AccessDenied,
                             QStringLiteral("Access is denied."));
    session->requestList(1, QStringLiteral("/"), false);
    session->requestList(2, QStringLiteral("/"), false);
    session->requestList(3, QStringLiteral("/"), false);
    ASSERT_TRUE(spinUntil([&] { return failedSpy.size() >= 3; }, 5000));

    // Stacking a second password dialog on top of the first is the failure mode
    // the connect-time path already guards against; the listing path must not
    // reintroduce it.
    EXPECT_EQ(authSpy.size(), 1);
}

TEST(SmbListingErrors, APlainFailureIsReportedWithoutAskingForAPassword) {
    auto provider = std::make_shared<ListingProvider>();
    auto session = connectedSession(provider);
    QSignalSpy authSpy(session.get(), &NetworkSession::authRequired);
    QSignalSpy failedSpy(session.get(), &NetworkSession::listFailed);

    provider->setListOutcome(FileProvider::ListStatus::Failed,
                             QStringLiteral("The network path was not found."));
    session->requestList(1, QStringLiteral("/"), false);
    ASSERT_TRUE(spinUntil([&] { return failedSpy.size() > 0; }, 5000));

    // Prompting for a password when the host is simply unreachable would be a
    // lie about what went wrong.
    EXPECT_EQ(authSpy.size(), 0);
    ASSERT_EQ(failedSpy.size(), 1);
    EXPECT_EQ(failedSpy.at(0).at(2).toString(),
              QStringLiteral("The network path was not found."));
}

TEST(SmbListingErrors, AnOkListingStillReportsAnEmptyDirectoryAsEmpty) {
    auto provider = std::make_shared<ListingProvider>();
    auto session = connectedSession(provider);
    QSignalSpy authSpy(session.get(), &NetworkSession::authRequired);
    QSignalSpy failedSpy(session.get(), &NetworkSession::listFailed);
    QSignalSpy readySpy(session.get(), &NetworkSession::listReady);

    // The status channel must not turn every empty directory into an error --
    // that would be the same bug with the sign flipped.
    provider->setListOutcome(FileProvider::ListStatus::Ok, QString());
    session->requestList(1, QStringLiteral("/"), false);
    ASSERT_TRUE(spinUntil([&] { return readySpy.size() > 0; }, 5000));
    EXPECT_EQ(failedSpy.size(), 0);
    EXPECT_EQ(authSpy.size(), 0);
}

TEST(SmbListingErrors, ProvidersThatCannotTellDefaultToReportingSuccess) {
    // Every backend that has not been taught the distinction keeps its previous
    // behaviour rather than being treated as permanently failing.
    ListingProvider bare;
    EXPECT_EQ(FileProvider::ListStatus::Ok, bare.lastListStatus());
    EXPECT_TRUE(bare.lastListError().isEmpty());
}

#ifdef Q_OS_WIN

TEST(WindowsSmbStatus, CredentialRejectionsAreDistinguishedFromDeadHosts) {
    using Result = WindowsSmbSession::Result;

    // Answerable by a password -- the UI prompts.
    for (unsigned long status : {static_cast<unsigned long>(ERROR_ACCESS_DENIED),
                                 static_cast<unsigned long>(ERROR_LOGON_FAILURE),
                                 static_cast<unsigned long>(ERROR_INVALID_PASSWORD),
                                 static_cast<unsigned long>(ERROR_NO_SUCH_USER),
                                 static_cast<unsigned long>(ERROR_ACCOUNT_DISABLED),
                                 static_cast<unsigned long>(ERROR_PASSWORD_EXPIRED),
                                 static_cast<unsigned long>(ERROR_ACCOUNT_LOCKED_OUT),
                                 static_cast<unsigned long>(ERROR_LOGON_TYPE_NOT_GRANTED)}) {
        EXPECT_EQ(Result::AuthRequired, WindowsSmbSession::classify(status)) << status;
    }

    // Not answerable by a password -- prompting would misdescribe the failure.
    for (unsigned long status : {static_cast<unsigned long>(ERROR_BAD_NETPATH),
                                 static_cast<unsigned long>(ERROR_BAD_NET_NAME),
                                 static_cast<unsigned long>(ERROR_NETWORK_UNREACHABLE),
                                 static_cast<unsigned long>(ERROR_NO_NETWORK),
                                 static_cast<unsigned long>(ERROR_SEM_TIMEOUT)}) {
        EXPECT_EQ(Result::Failed, WindowsSmbSession::classify(status)) << status;
    }
}

TEST(WindowsSmbStatus, AnExistingWindowsSessionCountsAsConnected) {
    using Result = WindowsSmbSession::Result;
    EXPECT_EQ(Result::Connected, WindowsSmbSession::classify(NO_ERROR));
    EXPECT_EQ(Result::Connected, WindowsSmbSession::classify(ERROR_ALREADY_ASSIGNED));
    EXPECT_EQ(Result::Connected, WindowsSmbSession::classify(ERROR_ALREADY_EXISTS));
    // The one that bites in practice: Windows refuses a second session to the
    // same server under a different user name. Treating it as an auth failure
    // would prompt for a password Windows will not apply anyway, while the
    // session that already exists is perfectly usable.
    EXPECT_EQ(Result::Connected,
              WindowsSmbSession::classify(ERROR_SESSION_CREDENTIAL_CONFLICT));
}

TEST(WindowsSmbProviderConnection, ReportsNoConnectionUntilOneIsEstablished) {
    WindowsSmbProvider provider;
    // Used to be `!m_host.isEmpty()`, which was true the moment a name was
    // stored -- so a connection that never happened reported itself as live.
    EXPECT_FALSE(provider.isConnected());
    EXPECT_FALSE(provider.isDir(QStringLiteral("/")));
    EXPECT_FALSE(provider.exists(QStringLiteral("/")));
}

TEST(WindowsSmbProviderConnection, ARejectedHostNameNeitherConnectsNorClaimsAuthFailure) {
    WindowsSmbProvider provider;
    QString error;
    // A syntactically impossible host is rejected before any dial; that is not a
    // credential problem, so it must not raise a password prompt.
    EXPECT_FALSE(provider.connectToHost(QStringLiteral("bad\\host"), QString(), QString(),
                                        QString(), true, &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_FALSE(provider.lastConnectAuthFailed());
    EXPECT_FALSE(provider.isConnected());
}

TEST(WindowsSmbProviderConnection, ListingRefusesTraversalAndSaysWhy) {
    WindowsSmbProvider provider;
    EXPECT_EQ(FileProvider::ListStatus::Ok, provider.lastListStatus());

    const QVector<FileInfo> entries = provider.list(QStringLiteral("/share/../.."), false);
    EXPECT_TRUE(entries.isEmpty());
    // An empty vector on its own would have read as "this directory is empty".
    EXPECT_EQ(FileProvider::ListStatus::Failed, provider.lastListStatus());
    EXPECT_FALSE(provider.lastListError().isEmpty());
}

#endif // Q_OS_WIN
