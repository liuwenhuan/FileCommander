#include <gtest/gtest.h>

#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QPlainTextEdit>
#include <QTemporaryDir>
#include <QTest>
#include <QTextCursor>
#include <QWidget>

#include "CommandRegistry.h"
#include "QuickView.h"
#include "TryUntil.h"
#include "config/Settings.h"

TEST(QuickViewClipboard, CtrlCCopiesSelectedPreviewTextInsteadOfThePanelPath) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("preview.txt"));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("selected text\nsecond line\n");
    file.close();

    // MainWindow's configurable commands are QShortcuts created by this exact
    // registry. Put the same Ctrl+C file command and QuickView in one window to
    // exercise the collision without constructing the rest of MainWindow.
    QWidget host;
    host.resize(640, 480);
    CommandRegistry commands(&host);
    commands.bind(QStringLiteral("copyClipboard"), QStringLiteral("Copy to Clipboard"),
                  QKeySequence::Copy, QKeySequence::Copy, [] {
                      if (CommandRegistry::invokeFocusedWidgetCommand("copy"))
                          return;
                      QApplication::clipboard()->setText(QStringLiteral("panel-file-path"));
                  });

    Settings settings(dir.filePath(QStringLiteral("viewer-settings.ini")));
    QuickView view(settings, QuickView::Context::Embedded, &host);
    view.resize(600, 400);
    view.showFile(path);
    view.show();
    host.show();

    auto *preview = view.findChild<QPlainTextEdit *>(QStringLiteral("quickViewTextView"));
    ASSERT_NE(preview, nullptr);
    FC_TRY_VERIFY_WITH_TIMEOUT(preview->toPlainText().startsWith(QStringLiteral("selected text")),
                               5000);

    QTextCursor selection(preview->document());
    selection.setPosition(0);
    selection.setPosition(QStringLiteral("selected text").size(), QTextCursor::KeepAnchor);
    preview->setTextCursor(selection);
    preview->setFocus();
    host.activateWindow();
    qApp->processEvents();
    ASSERT_EQ(QApplication::focusWidget(), preview);

    // Prove the preview control itself can copy this selection; the failure is
    // specifically the keyboard shortcut path, not read-only text behavior.
    QApplication::clipboard()->setText(QStringLiteral("direct-copy-sentinel"));
    preview->copy();
    ASSERT_EQ(QApplication::clipboard()->text(), QStringLiteral("selected text"));

    QApplication::clipboard()->setText(QStringLiteral("sentinel"));
    QTest::keyClick(preview, Qt::Key_C, Qt::ControlModifier);
    qApp->processEvents();

    EXPECT_EQ(QApplication::clipboard()->text(), QStringLiteral("selected text"));
}
