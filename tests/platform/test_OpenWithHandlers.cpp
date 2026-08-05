#include <gtest/gtest.h>

#include <QFileInfo>
#include <QFile>
#include <QTemporaryDir>

#include <iostream>

#include "OpenWithHandlers.h"

namespace {

fc::OpenWithHandler make(const QString &name, const QString &program, const QString &token = {},
                         bool recommended = false) {
    fc::OpenWithHandler handler;
    handler.displayName = name;
    handler.program = program;
    handler.token = token;
    handler.recommended = recommended;
    return handler;
}

QStringList namesOf(const QVector<fc::OpenWithHandler> &handlers) {
    QStringList names;
    for (const fc::OpenWithHandler &handler : handlers)
        names << handler.displayName;
    return names;
}

} // namespace

// The system lists the same application more than once -- Windows returns it
// under both the recommended and the full filter, and on Linux a user's
// .desktop file shadows the system copy. One menu entry each.
TEST(OpenWithHandlersTest, TheSameApplicationAppearsOnce) {
    QVector<fc::OpenWithHandler> raw{
        make(QStringLiteral("VLC"), QStringLiteral("C:/vlc/vlc.exe"), {}, true),
        make(QStringLiteral("VLC media player"), QStringLiteral("C:/VLC/VLC.EXE")),
    };
    const auto tidy = fc::tidyOpenWithHandlers(raw);
    ASSERT_EQ(tidy.size(), 1);
    EXPECT_EQ(tidy.first().displayName, QStringLiteral("VLC"));
}

// Two different applications that happen to share a display name are two
// entries -- deduplicating by name would silently hide one of them.
TEST(OpenWithHandlersTest, ADuplicateNameIsNotADuplicateApplication) {
    QVector<fc::OpenWithHandler> raw{
        make(QStringLiteral("Player"), QStringLiteral("C:/one/player.exe")),
        make(QStringLiteral("Player"), QStringLiteral("C:/two/player.exe")),
    };
    EXPECT_EQ(fc::tidyOpenWithHandlers(raw).size(), 2);
}

// Being listed for this file's type anywhere promotes the entry, whichever
// registration was seen first.
TEST(OpenWithHandlersTest, RecommendedWinsOverTheOrderOfDiscovery) {
    QVector<fc::OpenWithHandler> raw{
        make(QStringLiteral("Paint"), QStringLiteral("C:/paint.exe")),
        make(QStringLiteral("Paint"), QStringLiteral("C:/paint.exe"), {}, true),
    };
    const auto tidy = fc::tidyOpenWithHandlers(raw);
    ASSERT_EQ(tidy.size(), 1);
    EXPECT_TRUE(tidy.first().recommended);
}

// The menu shows the applications registered for the type first; everything
// else follows, and both halves read alphabetically.
TEST(OpenWithHandlersTest, RecommendedFirstThenAlphabetical) {
    QVector<fc::OpenWithHandler> raw{
        make(QStringLiteral("Zebra"), QStringLiteral("C:/z.exe")),
        make(QStringLiteral("photos"), QStringLiteral("C:/p.exe"), {}, true),
        make(QStringLiteral("Ant"), QStringLiteral("C:/a.exe")),
        make(QStringLiteral("Movies"), QStringLiteral("C:/m.exe"), {}, true),
    };
    EXPECT_EQ(namesOf(fc::tidyOpenWithHandlers(raw)),
              (QStringList{QStringLiteral("Movies"), QStringLiteral("photos"),
                           QStringLiteral("Ant"), QStringLiteral("Zebra")}));
}

TEST(OpenWithHandlersTest, EntriesWithNothingToShowOrRunAreDropped) {
    QVector<fc::OpenWithHandler> raw{
        make(QString(), QString()),
        make(QStringLiteral("Real"), QStringLiteral("C:/real.exe")),
    };
    const auto tidy = fc::tidyOpenWithHandlers(raw);
    ASSERT_EQ(tidy.size(), 1);
    EXPECT_EQ(tidy.first().displayName, QStringLiteral("Real"));
}

// A name with no extension.
//
// The two platforms genuinely disagree here, and the test says which rather
// than pretending one answer is universal. Windows associates by EXTENSION, so
// a name without one has nothing to look up and the enumeration is empty. XDG
// associates by MIME TYPE, which QMimeDatabase derives from the content and the
// name together, so an extensionless file still resolves -- to text/plain for
// an empty file -- and the handlers for that type are a correct answer.
//
// What must hold on both is that nothing malformed comes back.
TEST(OpenWithHandlersTest, AnExtensionlessNameIsHandledPerPlatformConvention) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("plain"));
    const auto handlers = fc::openWithHandlers(path);
#ifdef Q_OS_WIN
    EXPECT_TRUE(handlers.isEmpty()) << "Windows has no extension to associate by";
#endif
    for (const fc::OpenWithHandler &handler : handlers) {
        EXPECT_FALSE(handler.displayName.isEmpty());
        EXPECT_FALSE(handler.program.isEmpty() && handler.token.isEmpty());
    }
}

// The real enumeration, on whatever this machine has installed. It cannot
// assert on particular applications, but it can assert the shape of what comes
// back -- and it prints the list, which is how the menu was sized.
TEST(OpenWithHandlersTest, TheSystemOffersSomethingForACommonType) {
    // A real path on this machine, not a Windows-shaped literal: the XDG
    // implementation asks QMimeDatabase, which looks at the file as well as
    // the name.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString probe = dir.filePath(QStringLiteral("example.txt"));
    QFile file(probe);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("hello");
    file.close();

    const auto handlers = fc::openWithHandlers(probe);
    if (handlers.isEmpty())
        GTEST_SKIP() << "this system registers no handler for .txt";

    int recommended = 0;
    for (const fc::OpenWithHandler &handler : handlers) {
        EXPECT_FALSE(handler.displayName.isEmpty());
        EXPECT_FALSE(handler.program.isEmpty() && handler.token.isEmpty())
            << handler.displayName.toStdString() << " can neither be run nor invoked";
        if (handler.recommended)
            ++recommended;
        std::cerr << "OPENWITH " << (handler.recommended ? "* " : "  ")
                  << handler.displayName.toStdString() << "  |  "
                  << handler.program.toStdString() << "  |  " << handler.token.toStdString()
                  << std::endl;
    }
    // A bare CI container has applications installed but no desktop database
    // claiming text/plain, so "something is registered for this type" is not a
    // property of every machine. Skipped rather than asserted there, because
    // the alternative is a test that fails for a reason the code cannot fix.
    if (recommended == 0)
        GTEST_SKIP() << "no application on this machine claims text/plain";
    // The recommended ones lead, so a menu can put a separator after them.
    for (int i = 1; i < handlers.size(); ++i)
        EXPECT_FALSE(handlers[i].recommended && !handlers[i - 1].recommended)
            << "recommended entries are not contiguous at index " << i;
}
