#include <gtest/gtest.h>

#include <QComboBox>
#include <QFile>
#include <QLineEdit>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QToolBar>

#include "QuickView.h"
#include "TextEncodingDetector.h"
#include "TryUntil.h"
#include "config/Settings.h"

// The markdown page carries the same three controls as the text page: what the
// bytes were decoded as, find, and Edit on the far right.

namespace {

QString writeFile(const QTemporaryDir &dir, const QString &name, const QByteArray &bytes) {
    const QString path = dir.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return {};
    f.write(bytes);
    return path;
}

} // namespace

TEST(QuickViewMarkdownToolbar, DetectsTheEncodingAndReDecodesOnDemand) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    // GBK bytes for "中文标题" under a heading -- not valid UTF-8, so a wrong
    // decode is visible in the rendered text.
    QByteArray gbk("# ");
    gbk.append("\xD6\xD0\xCE\xC4", 4); // 中文
    gbk.append('\n');
    const QString path = writeFile(dir, QStringLiteral("doc.md"), gbk);
    ASSERT_FALSE(path.isEmpty());

    Settings settings(dir.filePath(QStringLiteral("settings.ini")));
    QuickView view(settings, QuickView::Context::Window);
    view.resize(600, 400);
    view.show();

    auto *combo = view.findChild<QComboBox *>(QStringLiteral("markdownEncodingCombo"));
    ASSERT_NE(combo, nullptr);
    auto *browser = view.findChild<QTextBrowser *>();
    ASSERT_NE(browser, nullptr);

    view.showFile(path);
    // The load is a background probe; nothing is on screen until it lands.
    FC_TRY_VERIFY_WITH_TIMEOUT(!browser->toPlainText().trimmed().isEmpty(), 5000);
    EXPECT_EQ(combo->currentIndex(), 0);
    EXPECT_TRUE(combo->itemText(0).startsWith(QStringLiteral("Auto (")))
        << combo->itemText(0).toStdString();
    EXPECT_EQ(browser->toPlainText().trimmed(), QString::fromUtf8("中文"));

    // Forcing a wrong codec must actually re-decode the bytes already in hand.
    const int latin1 = combo->findText(QStringLiteral("ISO-8859-1"));
    ASSERT_GE(latin1, 0);
    combo->setCurrentIndex(latin1);
    FC_TRY_VERIFY_WITH_TIMEOUT(browser->toPlainText().trimmed() != QString::fromUtf8("中文"), 5000);
}

TEST(QuickViewMarkdownToolbar, FindAndEditActOnTheMarkdownPage) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeFile(dir, QStringLiteral("doc.md"), "# Title\n\nneedle here\n");
    ASSERT_FALSE(path.isEmpty());

    Settings settings(dir.filePath(QStringLiteral("settings.ini")));
    QuickView view(settings, QuickView::Context::Window);
    view.resize(600, 400);
    view.show();
    view.showFile(path);

    auto *browser = view.findChild<QTextBrowser *>();
    ASSERT_NE(browser, nullptr);
    FC_TRY_VERIFY_WITH_TIMEOUT(browser->toPlainText().contains(QStringLiteral("needle")), 5000);

    auto *find = view.findChild<QLineEdit *>(QStringLiteral("markdownFindField"));
    ASSERT_NE(find, nullptr);
    find->setText(QStringLiteral("needle"));
    view.findNext();
    EXPECT_EQ(browser->textCursor().selectedText(), QStringLiteral("needle"));

    // Edit is the far-right action of the markdown toolbar, and it asks for the
    // same in-place switch the text page's button does.
    auto *toolbar = view.findChild<QToolBar *>(QStringLiteral("markdownToolbar"));
    ASSERT_NE(toolbar, nullptr);
    ASSERT_FALSE(toolbar->actions().isEmpty());
    QAction *edit = toolbar->actions().last();
    QSignalSpy requested(&view, &QuickView::editRequested);
    edit->trigger();
    ASSERT_EQ(requested.size(), 1);
    EXPECT_EQ(requested.at(0).at(0).toString(), path);
}

TEST(QuickViewMarkdownToolbar, RemembersManualEncodingAndAutoClearsIt) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeFile(dir, QStringLiteral("remember.md"),
                                   QByteArray::fromHex("2320D6D0CEC40A"));
    ASSERT_FALSE(path.isEmpty());
    Settings settings(dir.filePath(QStringLiteral("settings.ini")));
    const QString identity = QStringLiteral("stable-markdown-identity");

    int latin1 = -1;
    {
        QuickView view(settings, QuickView::Context::Window);
        auto *combo = view.findChild<QComboBox *>(QStringLiteral("markdownEncodingCombo"));
        auto *browser = view.findChild<QTextBrowser *>();
        ASSERT_NE(combo, nullptr);
        ASSERT_NE(browser, nullptr);
        view.showFile(path, identity);
        FC_TRY_VERIFY_WITH_TIMEOUT(!browser->toPlainText().trimmed().isEmpty(), 5000);
        latin1 = combo->findText(QStringLiteral("ISO-8859-1"));
        ASSERT_GT(latin1, 0);
        combo->setCurrentIndex(latin1);
        EXPECT_EQ(settings.rememberedTextEncodingIndex(identity), latin1);
    }

    QuickView reopened(settings, QuickView::Context::Window);
    auto *combo = reopened.findChild<QComboBox *>(QStringLiteral("markdownEncodingCombo"));
    auto *browser = reopened.findChild<QTextBrowser *>();
    ASSERT_NE(combo, nullptr);
    ASSERT_NE(browser, nullptr);
    reopened.showFile(path, identity);
    FC_TRY_VERIFY_WITH_TIMEOUT(!browser->toPlainText().trimmed().isEmpty(), 5000);
    EXPECT_EQ(combo->currentIndex(), latin1);
    combo->setCurrentIndex(TextEncodingDetector::autoEncodingIndex);
    EXPECT_EQ(settings.rememberedTextEncodingIndex(identity),
              TextEncodingDetector::autoEncodingIndex);
}
