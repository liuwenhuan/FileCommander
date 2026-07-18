#include "CompressDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFormLayout>
#include <QLineEdit>
#include <QVBoxLayout>

CompressDialog::CompressDialog(const QString &destDir, const QString &defaultBaseName,
                                QWidget *parent)
    : QDialog(parent), m_destDir(destDir) {
    setWindowTitle(tr("Compress"));

    m_nameEdit = new QLineEdit(defaultBaseName, this);
    m_formatCombo = new QComboBox(this);
    m_formatCombo->addItem(QStringLiteral("zip"), QStringLiteral("zip"));
    m_formatCombo->addItem(QStringLiteral("tar.gz"), QStringLiteral("tar.gz"));
    m_formatCombo->addItem(QStringLiteral("tar.bz2"), QStringLiteral("tar.bz2"));
    m_formatCombo->addItem(QStringLiteral("tar.xz"), QStringLiteral("tar.xz"));
    m_formatCombo->addItem(QStringLiteral("tar"), QStringLiteral("tar"));

    auto *form = new QFormLayout;
    form->addRow(tr("Archive name:"), m_nameEdit);
    form->addRow(tr("Format:"), m_formatCombo);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);
}

QString CompressDialog::format() const {
    return m_formatCombo->currentData().toString();
}

QString CompressDialog::archivePath() const {
    return QDir(m_destDir).filePath(m_nameEdit->text() + QLatin1Char('.') + format());
}
