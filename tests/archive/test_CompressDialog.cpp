#include <gtest/gtest.h>

#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>

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

    auto *level = dialog.findChild<QSpinBox *>();
    auto *password = passwordEdit(dialog);
    ASSERT_NE(level, nullptr);
    ASSERT_NE(password, nullptr);
    ASSERT_NE(levelLabel(dialog), nullptr);

    selectFormat(dialog, QStringLiteral("tar.gz"));
    EXPECT_TRUE(level->isVisible());
    EXPECT_TRUE(levelLabel(dialog)->isVisible());
    EXPECT_TRUE(level->isEnabled());
    EXPECT_FALSE(password->isVisible())
        << "tar.gz can tune compression but must not inherit ZIP password controls";

    level->setValue(9);
    EXPECT_EQ(dialog.compressionLevel(), 9);

    selectFormat(dialog, QStringLiteral("tar"));
    EXPECT_FALSE(level->isVisible());
    EXPECT_FALSE(levelLabel(dialog)->isVisible());
}

TEST(CompressDialogTest, EveryCompressedTarFormatExposesAnEditableLevel) {
    CompressDialog dialog(QStringLiteral("/tmp"), QStringLiteral("archive"));
    dialog.show();
    QCoreApplication::processEvents();

    auto *level = dialog.findChild<QSpinBox *>();
    ASSERT_NE(level, nullptr);

    for (const QString &format : {QStringLiteral("tar.gz"), QStringLiteral("tar.bz2"),
                                  QStringLiteral("tar.xz")}) {
        selectFormat(dialog, format);
        EXPECT_TRUE(level->isVisible()) << format.toStdString();
        EXPECT_TRUE(level->isEnabled()) << format.toStdString();
        level->setValue(3);
        EXPECT_EQ(dialog.compressionLevel(), 3) << format.toStdString();
    }
}
