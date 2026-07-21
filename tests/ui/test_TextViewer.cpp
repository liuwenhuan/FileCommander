#include <gtest/gtest.h>

#include <QByteArray>

#include "QuickView.h"

// toHexDump is pure formatting and needs no QApplication.

TEST(TextViewerTest, HexDumpFormatsOffsetHexAndAscii) {
    const QString dump = QuickView::toHexDump(QByteArray("Hi"));
    EXPECT_TRUE(dump.startsWith("00000000  48 69 ")) << dump.toStdString();
    EXPECT_TRUE(dump.contains(" Hi\n")) << dump.toStdString();
}

TEST(TextViewerTest, HexDumpRendersNonPrintablesAsDots) {
    QByteArray data;
    data.append('\x00');
    data.append('\x41'); // 'A'
    const QString dump = QuickView::toHexDump(data);
    EXPECT_TRUE(dump.contains("00 41 ")) << dump.toStdString();
    EXPECT_TRUE(dump.contains(".A")) << dump.toStdString();
}

TEST(TextViewerTest, HexDumpEmptyForEmptyInput) {
    EXPECT_TRUE(QuickView::toHexDump(QByteArray()).isEmpty());
}
