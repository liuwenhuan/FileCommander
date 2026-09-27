#include <gtest/gtest.h>

#include <QApplication>
#include <QLabel>
#include <QProgressBar>
#include <QTest>
#include <QTranslator>

#include "DialogTitleBar.h"
#include "IncomingTransferWindow.h"

namespace {

QLabel *label(IncomingTransferWindow &window, const char *name) {
    return window.findChild<QLabel *>(QString::fromLatin1(name));
}

QProgressBar *progress(IncomingTransferWindow &window) {
    return window.findChild<QProgressBar *>(QStringLiteral("IncomingTransferProgress"));
}

} // namespace

class IncomingTestTranslator : public QTranslator {
public:
    bool isEmpty() const override { return false; }
    QString translate(const char *context, const char *sourceText,
                      const char *, int) const override {
        if (qstrcmp(context, "IncomingTransferWindow") != 0)
            return {};
        if (qstrcmp(sourceText, "Receiving file") == 0)
            return QStringLiteral("Translated window");
        if (qstrcmp(sourceText, "Receiving") == 0)
            return QStringLiteral("Translated receiving");
        return {};
    }
};

TEST(IncomingTransferWindowTest, OpenWindowRetranslatesWhenLanguageChanges) {
    IncomingTestTranslator translator;
    IncomingTransferWindow window;
    window.updateTransfer(QStringLiteral("one"), QStringLiteral("a.bin"), 1, 4,
                          QStringLiteral("receiving"));
    ASSERT_TRUE(qApp->installTranslator(&translator));
    qApp->processEvents();
    EXPECT_EQ(window.windowTitle(), QStringLiteral("Translated window"));
    EXPECT_EQ(label(window, "IncomingTransferState")->text(),
              QStringLiteral("Translated receiving"));
    qApp->removeTranslator(&translator);
}

TEST(IncomingTransferWindowTest, ReceivingTransferShowsWrittenBytesInManagedWindow) {
    IncomingTransferWindow window;
    EXPECT_FALSE(window.isVisible());

    window.updateTransfer(QStringLiteral("one"), QStringLiteral("video.bin"), 1024, 4096,
                          QStringLiteral("receiving"));
    qApp->processEvents();

    EXPECT_TRUE(window.isVisible());
    EXPECT_EQ(window.windowFlags() & Qt::WindowType_Mask, Qt::Window);
    EXPECT_TRUE(window.windowFlags().testFlag(Qt::WindowMinimizeButtonHint));
    ASSERT_NE(window.titleBar(), nullptr);
    EXPECT_EQ(window.titleBar()->controls(), DialogTitleBar::WindowControls);
    ASSERT_NE(label(window, "IncomingTransferFile"), nullptr);
    EXPECT_TRUE(label(window, "IncomingTransferFile")->text().contains(QStringLiteral("video.bin")));
    ASSERT_NE(label(window, "IncomingTransferBytes"), nullptr);
    EXPECT_TRUE(label(window, "IncomingTransferBytes")->text().contains(QStringLiteral("1.0 KB")));
    ASSERT_NE(progress(window), nullptr);
    EXPECT_EQ(progress(window)->value(), 250);
}

TEST(IncomingTransferWindowTest, UpdatesDoNotRestoreMinimizedWindow) {
    IncomingTransferWindow window;
    window.updateTransfer(QStringLiteral("one"), QStringLiteral("a.bin"), 0, 4096,
                          QStringLiteral("receiving"));
    window.showMinimized();
    qApp->processEvents();
    ASSERT_TRUE(window.isMinimized());

    window.updateTransfer(QStringLiteral("one"), QStringLiteral("a.bin"), 2048, 4096,
                          QStringLiteral("receiving"));
    qApp->processEvents();

    EXPECT_TRUE(window.isMinimized());
    ASSERT_NE(progress(window), nullptr);
    EXPECT_EQ(progress(window)->value(), 500);
}

TEST(IncomingTransferWindowTest, ShowingTransferDoesNotTakeFocusFromAnotherWindow) {
    QWidget foreground;
    foreground.show();
    foreground.activateWindow();
    qApp->processEvents();
    ASSERT_EQ(qApp->activeWindow(), &foreground);

    IncomingTransferWindow window;
    window.updateTransfer(QStringLiteral("one"), QStringLiteral("a.bin"), 0, 4096,
                          QStringLiteral("receiving"));
    qApp->processEvents();

    ASSERT_TRUE(window.isVisible());
    EXPECT_TRUE(window.testAttribute(Qt::WA_ShowWithoutActivating));
    if (QGuiApplication::platformName() != QStringLiteral("offscreen"))
        EXPECT_EQ(qApp->activeWindow(), &foreground);

    foreground.activateWindow();
    qApp->processEvents();
    ASSERT_EQ(qApp->activeWindow(), &foreground);
    window.updateTransfer(QStringLiteral("one"), QStringLiteral("a.bin"), 2048, 4096,
                          QStringLiteral("receiving"));
    qApp->processEvents();
    EXPECT_EQ(qApp->activeWindow(), &foreground);
}

TEST(IncomingTransferWindowTest, RateFallsToZeroWhenWritesStall) {
    IncomingTransferWindow window;
    window.updateTransfer(QStringLiteral("one"), QStringLiteral("a.bin"), 0, 8192,
                          QStringLiteral("receiving"));
    QTest::qWait(300);
    window.updateTransfer(QStringLiteral("one"), QStringLiteral("a.bin"), 4096, 8192,
                          QStringLiteral("receiving"));

    ASSERT_NE(label(window, "IncomingTransferRate"), nullptr);
    EXPECT_FALSE(label(window, "IncomingTransferRate")->text().contains(QStringLiteral("0 B/s")));
    QTest::qWait(2300);
    EXPECT_TRUE(label(window, "IncomingTransferRate")->text().contains(QStringLiteral("0 B/s")));
}

TEST(IncomingTransferWindowTest, TerminalStatesStopRateAndNewTransferResetsProgress) {
    IncomingTransferWindow window;
    window.updateTransfer(QStringLiteral("one"), QStringLiteral("a.bin"), 2048, 4096,
                          QStringLiteral("receiving"));
    window.updateTransfer(QStringLiteral("one"), QStringLiteral("a.bin"), 4096, 4096,
                          QStringLiteral("complete"));
    ASSERT_NE(label(window, "IncomingTransferState"), nullptr);
    EXPECT_TRUE(label(window, "IncomingTransferState")->text().contains(QStringLiteral("Complete")));
    EXPECT_EQ(progress(window)->value(), 1000);
    EXPECT_TRUE(label(window, "IncomingTransferRate")->text().contains(QStringLiteral("0 B/s")));

    window.updateTransfer(QStringLiteral("two"), QStringLiteral("b.bin"), 512, -1,
                          QStringLiteral("receiving"));
    EXPECT_EQ(progress(window)->minimum(), 0);
    EXPECT_EQ(progress(window)->maximum(), 0);
    EXPECT_TRUE(label(window, "IncomingTransferFile")->text().contains(QStringLiteral("b.bin")));
    window.updateTransfer(QStringLiteral("two"), QStringLiteral("b.bin"), 512, -1,
                          QStringLiteral("failed"));
    EXPECT_TRUE(label(window, "IncomingTransferState")->text().contains(QStringLiteral("Failed")));
}

TEST(IncomingTransferWindowTest, TerminalUpdateForUnknownTransferDoesNotOpenWindow) {
    IncomingTransferWindow window;
    window.updateTransfer(QStringLiteral("one"), QStringLiteral("a.bin"), 4096, 4096,
                          QStringLiteral("complete"));
    EXPECT_FALSE(window.isVisible());
}

TEST(IncomingTransferWindowTest, LargeTotalsKeepCorrectProgressFraction) {
    IncomingTransferWindow window;
    const qint64 total = 1LL << 60;
    window.updateTransfer(QStringLiteral("one"), QStringLiteral("large.bin"), total / 2, total,
                          QStringLiteral("receiving"));
    ASSERT_NE(progress(window), nullptr);
    EXPECT_EQ(progress(window)->value(), 500);
}

TEST(IncomingTransferWindowTest, FailedTransferDoesNotLookComplete) {
    IncomingTransferWindow window;
    window.updateTransfer(QStringLiteral("one"), QStringLiteral("a.bin"), 4096, 4096,
                          QStringLiteral("receiving"));
    window.updateTransfer(QStringLiteral("one"), QStringLiteral("a.bin"), 4096, 4096,
                          QStringLiteral("failed"));
    ASSERT_NE(progress(window), nullptr);
    EXPECT_LT(progress(window)->value(), progress(window)->maximum());
}

TEST(IncomingTransferWindowTest, InterruptedTransferStopsRateWithoutRestoringWindow) {
    IncomingTransferWindow window;
    window.updateTransfer(QStringLiteral("one"), QStringLiteral("a.bin"), 2048, 4096,
                          QStringLiteral("receiving"));
    window.showMinimized();
    qApp->processEvents();
    ASSERT_TRUE(window.isMinimized());

    window.updateTransfer(QStringLiteral("one"), QStringLiteral("a.bin"), 2048, 4096,
                          QStringLiteral("interrupted"));
    qApp->processEvents();

    EXPECT_TRUE(window.isMinimized());
    ASSERT_NE(label(window, "IncomingTransferState"), nullptr);
    EXPECT_TRUE(label(window, "IncomingTransferState")->text().contains(QStringLiteral("Interrupted")));
    EXPECT_TRUE(label(window, "IncomingTransferRate")->text().contains(QStringLiteral("0 B/s")));
    ASSERT_NE(progress(window), nullptr);
    EXPECT_LT(progress(window)->value(), progress(window)->maximum());
}
