#include <gtest/gtest.h>

#include <QDateTime>

#include "network/WindowsSmbProvider.h"

namespace {

class RecordingWindowsSmbProvider : public WindowsSmbProvider {
public:
    RenameResult moveTo(const QString &srcPath, const QString &dstPath) override {
        sourcePath = srcPath;
        destinationPath = dstPath;
        return RenameResult::Unsupported;
    }

    QString sourcePath;
    QString destinationPath;
};

} // namespace

TEST(WindowsSmbProvider, ConvertsProviderPathsToCanonicalUnc) {
    QString error;
    EXPECT_EQ(WindowsSmbProvider::providerPathToUnc(
                  QStringLiteral("nas"), QStringLiteral("/"), &error),
              QStringLiteral("\\\\nas"));
    EXPECT_EQ(WindowsSmbProvider::providerPathToUnc(
                  QStringLiteral("nas"), QStringLiteral("/共享/目录/文件.txt"), &error),
              QStringLiteral("\\\\nas\\共享\\目录\\文件.txt"));
    EXPECT_TRUE(error.isEmpty());
}

TEST(WindowsSmbProvider, ShellAccessiblePathUsesCanonicalUnc) {
    WindowsSmbProvider provider;
    QString error;
    // The dial fails -- "nas" is not a real host here -- and that is fine for
    // this test: it is about path conversion, and the target stays configured
    // afterwards so reconnect() (and the credentialed retry the login prompt
    // drives) can still run. This used to assert success only because
    // connectToHost never touched the network at all.
    provider.connectToHost(QStringLiteral("nas"), {}, {}, {}, true, &error);
    EXPECT_FALSE(provider.isConnected());

    EXPECT_EQ(provider.shellAccessiblePath(QStringLiteral("/media/movie.mp4")),
              QStringLiteral("\\\\nas\\media\\movie.mp4"));
}

TEST(WindowsSmbProvider, RejectsTraversalAndInvalidHosts) {
    QString error;
    EXPECT_TRUE(WindowsSmbProvider::providerPathToUnc(
                    QStringLiteral("nas"), QStringLiteral("/share/../secret"), &error)
                    .isEmpty());
    EXPECT_FALSE(error.isEmpty());

    error.clear();
    EXPECT_TRUE(WindowsSmbProvider::providerPathToUnc(
                    QStringLiteral("nas\\other"), QStringLiteral("/share"), &error)
                    .isEmpty());
    EXPECT_FALSE(error.isEmpty());
}

TEST(WindowsSmbProvider, NormalizesProviderPathWithoutEscapingRoot) {
    EXPECT_EQ(WindowsSmbProvider::normalizeProviderPath(QStringLiteral("//share///dir/")),
              QStringLiteral("/share/dir"));
    EXPECT_EQ(WindowsSmbProvider::normalizeProviderPath(QStringLiteral("/../../share")),
              QStringLiteral("/share"));
    EXPECT_EQ(WindowsSmbProvider::normalizeProviderPath(QStringLiteral("\\share\\dir")),
              QStringLiteral("/share/dir"));
    EXPECT_EQ(WindowsSmbProvider::parentProviderPath(QStringLiteral("/share/dir")),
              QStringLiteral("/share"));
    EXPECT_EQ(WindowsSmbProvider::parentProviderPath(
                  QStringLiteral("/share/dir/../file")),
              QStringLiteral("/share"));
    EXPECT_TRUE(WindowsSmbProvider::parentProviderPath(QStringLiteral("/")).isEmpty());
}

TEST(WindowsSmbProvider, RenamePreservesSourceTraversalForMoveValidation) {
    const QString source = QStringLiteral("/share/../other/file");
    RecordingWindowsSmbProvider provider;

    EXPECT_EQ(provider.rename(source, QStringLiteral("renamed"), nullptr),
              FileProvider::RenameResult::Unsupported);
    EXPECT_EQ(provider.sourcePath, source);
    EXPECT_EQ(provider.destinationPath, QStringLiteral("/other/renamed"));

    WindowsSmbProvider validatingProvider;
    EXPECT_EQ(validatingProvider.moveTo(source, QStringLiteral("/other/renamed")),
              FileProvider::RenameResult::Unsupported);
}

TEST(WindowsSmbProvider, RejectsTraversalBeforeRootShortcuts) {
    WindowsSmbProvider provider;
    // No connect on purpose. Traversal is rejected before the path is used for
    // anything, so the guard must hold with no link at all -- and asserting it
    // that way keeps this test off the network entirely.
    EXPECT_FALSE(provider.isDir(QStringLiteral("/share/..")));
    EXPECT_FALSE(provider.exists(QStringLiteral("/share/..")));
}

TEST(WindowsSmbProvider, RejectsTraversalAcrossAccessAndMutationEntryPoints) {
    WindowsSmbProvider provider;
    const QString traversal = QStringLiteral("/share/../C$/secret");

    EXPECT_EQ(provider.openRead(traversal), nullptr);
    EXPECT_EQ(provider.openWrite(traversal, true), nullptr);
    EXPECT_FALSE(provider.setModifiedTime(traversal, QDateTime::currentDateTime()));
    EXPECT_FALSE(provider.remove(traversal));
    EXPECT_FALSE(provider.mkdir(traversal));
    EXPECT_EQ(provider.moveTo(traversal, QStringLiteral("/share/target")),
              FileProvider::RenameResult::Unsupported);
    EXPECT_EQ(provider.rename(traversal, QStringLiteral("renamed"), nullptr),
              FileProvider::RenameResult::Unsupported);
}
