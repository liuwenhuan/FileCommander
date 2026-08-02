#include <gtest/gtest.h>

#include "dialogs/ExternalConnectDialog.h"
#include "Typography.h"

#include <QApplication>
#include <QFile>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QSet>
#include <QToolButton>

namespace {

class StyleSheetRestore final {
public:
    StyleSheetRestore() : m_value(qApp->styleSheet()) {}
    ~StyleSheetRestore() { qApp->setStyleSheet(m_value); }

private:
    QString m_value;
};

QList<QLabel *> sectionHeaders(ExternalConnectDialog &dialog) {
    return dialog.findChildren<QLabel *>(QStringLiteral("ExternalConnectHeaderLabel"));
}

void expectUniformHeaderTypography(ExternalConnectDialog &dialog, const QFont &chromeFont) {
    const QList<QLabel *> headers = sectionHeaders(dialog);
    ASSERT_EQ(headers.size(), 3);

    const QFont reference = headers.first()->font();
    const int referenceLineHeight = QFontMetrics(reference).height();
    EXPECT_EQ(reference.family(), chromeFont.family());
    EXPECT_EQ(reference.pointSize(), chromeFont.pointSize());
    EXPECT_TRUE(reference.bold());

    for (QLabel *header : headers) {
        ASSERT_NE(header, nullptr);
        EXPECT_EQ(header->font().family(), reference.family());
        EXPECT_EQ(header->font().pointSize(), reference.pointSize());
        EXPECT_EQ(header->font().weight(), reference.weight());
        EXPECT_EQ(QFontMetrics(header->font()).height(), referenceLineHeight);
    }
}

void expectHeaderRowsFitTheirFonts(ExternalConnectDialog &dialog) {
    auto *list = dialog.findChild<QListWidget *>(QStringLiteral("ExternalConnectList"));
    ASSERT_NE(list, nullptr);

    const QList<QLabel *> headers = sectionHeaders(dialog);
    ASSERT_EQ(headers.size(), 3);

    QSet<int> rowHeights;
    for (QLabel *header : headers) {
        QWidget *row = header->parentWidget();
        ASSERT_NE(row, nullptr);

        QListWidgetItem *item = nullptr;
        for (int i = 0; i < list->count(); ++i) {
            if (list->itemWidget(list->item(i)) == row) {
                item = list->item(i);
                break;
            }
        }
        ASSERT_NE(item, nullptr);

        const int lineHeight = QFontMetrics(header->font()).height();
        const int minimumSafeHeight = lineHeight + 8;
        EXPECT_GE(item->sizeHint().height(), minimumSafeHeight);
        EXPECT_GE(list->sizeHintForRow(list->row(item)), minimumSafeHeight);
        EXPECT_GE(row->height(), lineHeight);
        EXPECT_GE(header->height(), lineHeight);
        rowHeights.insert(list->sizeHintForRow(list->row(item)));
    }

    EXPECT_EQ(rowHeights.size(), 1);
}

QString readTheme(const QString &name) {
    QFile file(QStringLiteral(TTC_SOURCE_DIR "/resources/themes/%1.qss").arg(name));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

QListWidgetItem *addDeviceRowFixture(ExternalConnectDialog &dialog) {
    auto *list = dialog.findChild<QListWidget *>(QStringLiteral("ExternalConnectList"));
    if (!list)
        return nullptr;

    auto *item = new QListWidgetItem(list);
    item->setData(Qt::UserRole, 1); // ExternalConnectDialog::KindDevice
    auto *row = new QWidget(list);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    auto *name = new QLabel(QStringLiteral("Portable drive"), row);
    layout->addWidget(name);
    layout->addStretch(1);
    layout->addWidget(new QToolButton(row));
    item->setSizeHint(row->sizeHint());
    list->setItemWidget(item, row);
    return item;
}

} // namespace

TEST(ExternalConnectDialogTest, AllSectionHeadersUseTheSameChromeTypography) {
    ExternalConnectDialog dialog(nullptr, nullptr);
    QFont chromeFont = QApplication::font();
    chromeFont.setPointSize(15);

    Typography::applyChromeFont(&dialog, chromeFont);
    QCoreApplication::processEvents();

    expectUniformHeaderTypography(dialog, chromeFont);
}

TEST(ExternalConnectDialogTest, SectionHeadersTrackRuntimeChromeFontChanges) {
    ExternalConnectDialog dialog(nullptr, nullptr);
    QFont initial = QApplication::font();
    initial.setPointSize(11);
    Typography::applyChromeFont(&dialog, initial);
    QCoreApplication::processEvents();
    expectUniformHeaderTypography(dialog, initial);

    QFont changed = initial;
    changed.setFamily(QStringLiteral("Arial"));
    changed.setPointSize(17);
    Typography::applyChromeFont(&dialog, changed);
    QCoreApplication::processEvents();

    expectUniformHeaderTypography(dialog, changed);
}

TEST(ExternalConnectDialogTest, DeviceRowSizeHintTracksRuntimeChromeFontChanges) {
    StyleSheetRestore restore;
    qApp->setStyleSheet(readTheme(QStringLiteral("green")));
    ExternalConnectDialog dialog(nullptr, nullptr);
    QFont initial = QApplication::font();
    initial.setPointSize(9);
    Typography::applyChromeFont(&dialog, initial);
    QCoreApplication::processEvents();

    auto *list = dialog.findChild<QListWidget *>(QStringLiteral("ExternalConnectList"));
    ASSERT_NE(list, nullptr);
    QListWidgetItem *item = addDeviceRowFixture(dialog);
    ASSERT_NE(item, nullptr);
    const int initialHeight = item->sizeHint().height();

    QFont changed = initial;
    changed.setPointSize(22);
    Typography::applyChromeFont(&dialog, changed);
    QCoreApplication::processEvents();

    QWidget *row = list->itemWidget(item);
    ASSERT_NE(row, nullptr);
    EXPECT_GT(row->sizeHint().height(), initialHeight);
    EXPECT_GE(item->sizeHint().height(), row->sizeHint().height());
    EXPECT_GE(list->sizeHintForRow(list->row(item)), row->sizeHint().height());
}

TEST(ExternalConnectDialogTest, SectionHeaderTypographyIsUniformInEveryTheme) {
    const QString originalStyleSheet = qApp->styleSheet();
    const QStringList themes = {QStringLiteral("green"), QStringLiteral("dark"),
                                QStringLiteral("light")};

    for (const QString &theme : themes) {
        const QString styleSheet = readTheme(theme);
        ASSERT_FALSE(styleSheet.isEmpty()) << theme.toStdString();
        qApp->setStyleSheet(styleSheet);

        ExternalConnectDialog dialog(nullptr, nullptr);
        QFont chromeFont = QApplication::font();
        chromeFont.setPointSize(14);
        Typography::applyChromeFont(&dialog, chromeFont);
        QCoreApplication::processEvents();

        auto *list = dialog.findChild<QListWidget *>(QStringLiteral("ExternalConnectList"));
        ASSERT_NE(list, nullptr);
        EXPECT_EQ(dialog.font().pointSize(), chromeFont.pointSize()) << theme.toStdString();
        EXPECT_EQ(list->font().pointSize(), chromeFont.pointSize()) << theme.toStdString();
        expectUniformHeaderTypography(dialog, chromeFont);
    }

    qApp->setStyleSheet(originalStyleSheet);
}

TEST(ExternalConnectDialogTest, SectionHeaderRowsTrackFontMetricsWithoutClipping) {
    const QString originalStyleSheet = qApp->styleSheet();
    const QStringList themes = {QStringLiteral("green"), QStringLiteral("dark"),
                                QStringLiteral("light")};
    const QList<int> pointSizes = {9, 14, 22};

    for (const QString &theme : themes) {
        const QString styleSheet = readTheme(theme);
        ASSERT_FALSE(styleSheet.isEmpty()) << theme.toStdString();
        qApp->setStyleSheet(styleSheet);

        for (const int pointSize : pointSizes) {
            ExternalConnectDialog dialog(nullptr, nullptr);
            QFont chromeFont = QApplication::font();
            chromeFont.setPointSize(pointSize);
            Typography::applyChromeFont(&dialog, chromeFont);
            dialog.show();
            QCoreApplication::processEvents();

            expectUniformHeaderTypography(dialog, chromeFont);
            expectHeaderRowsFitTheirFonts(dialog);
        }
    }

    qApp->setStyleSheet(originalStyleSheet);
}
