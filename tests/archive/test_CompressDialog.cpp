#include <gtest/gtest.h>

#include <QApplication>
#include <QComboBox>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QToolButton>

#include "CompressDialog.h"

namespace {

void selectFormat(CompressDialog &dialog, const QString &format) {
    auto *combo = dialog.findChild<QComboBox *>();
    ASSERT_NE(combo, nullptr);
    const int index = combo->findData(format);
    ASSERT_GE(index, 0);
    combo->setCurrentIndex(index);
    QCoreApplication::processEvents();
}

QLabel *levelLabel(CompressDialog &dialog) {
    for (QLabel *label : dialog.findChildren<QLabel *>()) {
        if (label->text().contains(QStringLiteral("Compression level")))
            return label;
    }
    return nullptr;
}

QLineEdit *passwordEdit(CompressDialog &dialog) {
    for (QLineEdit *edit : dialog.findChildren<QLineEdit *>()) {
        if (edit->echoMode() == QLineEdit::Password)
            return edit;
    }
    return nullptr;
}

} // namespace

TEST(CompressDialogTest, CompressionLevelIsIndependentOfPasswordOptions) {
    CompressDialog dialog(QStringLiteral("/tmp"), QStringLiteral("archive"));
    dialog.show();
    QCoreApplication::processEvents();

    auto *level = dialog.findChild<QLineEdit *>(QStringLiteral("CompressionLevelEdit"));
    auto *decrease = dialog.findChild<QToolButton *>(QStringLiteral("CompressionLevelDecrease"));
    auto *increase = dialog.findChild<QToolButton *>(QStringLiteral("CompressionLevelIncrease"));
    auto *password = passwordEdit(dialog);
    ASSERT_NE(level, nullptr);
    ASSERT_NE(decrease, nullptr);
    ASSERT_NE(increase, nullptr);
    ASSERT_NE(password, nullptr);
    ASSERT_NE(levelLabel(dialog), nullptr);

    EXPECT_EQ(level->text(), QStringLiteral("6"));
    EXPECT_EQ(level->alignment(), Qt::AlignCenter);
    EXPECT_TRUE(decrease->autoRaise());
    EXPECT_TRUE(increase->autoRaise());
    const auto *validator = qobject_cast<const QIntValidator *>(level->validator());
    ASSERT_NE(validator, nullptr);
    EXPECT_EQ(validator->bottom(), 0);
    EXPECT_EQ(validator->top(), 9);

    selectFormat(dialog, QStringLiteral("tar.gz"));
    EXPECT_TRUE(level->isVisible());
    EXPECT_TRUE(levelLabel(dialog)->isVisible());
    EXPECT_TRUE(level->isEnabled());
    EXPECT_FALSE(password->isVisible())
        << "tar.gz can tune compression but must not inherit ZIP password controls";

    level->clear();
    QCoreApplication::processEvents();
    EXPECT_EQ(dialog.compressionLevel(), 6);

    level->setText(QStringLiteral("9"));
    ASSERT_TRUE(QMetaObject::invokeMethod(level, "editingFinished", Qt::DirectConnection));
    EXPECT_EQ(dialog.compressionLevel(), 9);
    increase->click();
    EXPECT_EQ(dialog.compressionLevel(), 9);
    decrease->click();
    EXPECT_EQ(dialog.compressionLevel(), 8);

    selectFormat(dialog, QStringLiteral("tar"));
    EXPECT_FALSE(level->isVisible());
    EXPECT_FALSE(levelLabel(dialog)->isVisible());
}

TEST(CompressDialogTest, EveryCompressedTarFormatExposesAnEditableLevel) {
    CompressDialog dialog(QStringLiteral("/tmp"), QStringLiteral("archive"));
    dialog.show();
    QCoreApplication::processEvents();

    auto *level = dialog.findChild<QLineEdit *>(QStringLiteral("CompressionLevelEdit"));
    ASSERT_NE(level, nullptr);

    for (const QString &format : {QStringLiteral("tar.gz"), QStringLiteral("tar.bz2"),
                                  QStringLiteral("tar.xz")}) {
        selectFormat(dialog, format);
        EXPECT_TRUE(level->isVisible()) << format.toStdString();
        EXPECT_TRUE(level->isEnabled()) << format.toStdString();
        level->setText(QStringLiteral("3"));
        ASSERT_TRUE(QMetaObject::invokeMethod(level, "editingFinished", Qt::DirectConnection));
        EXPECT_EQ(dialog.compressionLevel(), 3) << format.toStdString();
    }
}
