#include "CompressDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QVBoxLayout>

namespace {
// Compression levels that make sense for most backends (0 = store, 9 = best).
constexpr int kDefaultLevel = 6;
// Minimum characters before a passphrase is accepted so an accidental stray
// keystroke doesn't silently produce an encrypted archive.
constexpr int kMinPassphraseLength = 1;

bool formatSupportsPassword(const QString &format) {
    return format == QLatin1String("zip") || format == QLatin1String("7z");
}

bool formatSupportsLevel(const QString &format) {
    return format != QLatin1String("tar"); // store-only, pure tar
}
} // namespace

CompressDialog::CompressDialog(const QString &destDir, const QString &defaultBaseName,
                                QWidget *parent)
    : FramelessDialog(parent), m_destDir(destDir) {
    setWindowTitle(tr("Compress"));

    m_nameEdit = new QLineEdit(defaultBaseName, this);
    m_formatCombo = new QComboBox(this);
    m_formatCombo->addItem(QStringLiteral("zip"), QStringLiteral("zip"));
    m_formatCombo->addItem(QStringLiteral("tar.gz"), QStringLiteral("tar.gz"));
    m_formatCombo->addItem(QStringLiteral("tar.bz2"), QStringLiteral("tar.bz2"));
    m_formatCombo->addItem(QStringLiteral("tar.xz"), QStringLiteral("tar.xz"));
    m_formatCombo->addItem(QStringLiteral("tar"), QStringLiteral("tar"));
    m_formatCombo->setCurrentIndex(0);

    // --- Password block -------------------------------------------
    m_passwordOptions = new QWidget(this);
    auto *passwordLayout = new QFormLayout(m_passwordOptions);
    passwordLayout->setContentsMargins(0, 4, 0, 0);

    m_passphraseEdit = new QLineEdit(m_passwordOptions);
    m_passphraseEdit->setEchoMode(QLineEdit::Password);
    m_passphraseEdit->setPlaceholderText(tr("Leave empty for no encryption"));
    passwordLayout->addRow(tr("Password:"), m_passphraseEdit);

    m_encryptHeadersCheck = new QCheckBox(tr("Encrypt file list too"), m_passwordOptions);
    m_encryptHeadersCheck->setToolTip(
        tr("When checked, individual file names inside the archive are encrypted. "
           "Uncheck to see the file list without the password (ZIP-style)."));
    // Only applicable to 7z header encryption.
    m_encryptHeadersCheck->setChecked(false);
    passwordLayout->addRow(m_encryptHeadersCheck);

    // Compression tuning belongs to its own block: formats such as tar.gz do
    // not support passwords but do support a compression level.
    m_levelOptions = new QWidget(this);
    auto *levelLayout = new QFormLayout(m_levelOptions);
    levelLayout->setContentsMargins(0, 4, 0, 0);
    m_levelLabel = new QLabel(tr("Compression level:"), m_levelOptions);
    m_levelSpinner = new QSpinBox(m_levelOptions);
    m_levelSpinner->setRange(0, 9);
    m_levelSpinner->setValue(kDefaultLevel);
    m_levelSpinner->setToolTip(tr("0 = store only, 9 = best compression"));
    levelLayout->addRow(m_levelLabel, m_levelSpinner);

    // --- Main form -----------------------------------------------
    auto *form = new QFormLayout;
    form->addRow(tr("Archive name:"), m_nameEdit);
    form->addRow(tr("Format:"), m_formatCombo);
    form->addRow(m_passwordOptions);
    form->addRow(m_levelOptions);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);

    connect(m_formatCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CompressDialog::onFormatChanged);
    onFormatChanged(m_formatCombo->currentIndex());
}

void CompressDialog::onFormatChanged(int /*index*/) {
    const QString fmt = format();
    const bool showPassword = formatSupportsPassword(fmt);
    m_passwordOptions->setVisible(showPassword);
    m_encryptHeadersCheck->setVisible(fmt == QLatin1String("7z"));

    const bool showLevel = formatSupportsLevel(fmt);
    m_levelOptions->setVisible(showLevel);
}

QString CompressDialog::format() const {
    return m_formatCombo->currentData().toString();
}

QString CompressDialog::archivePath() const {
    const QString ext = format();
    // tar.gz / tar.bz2 / tar.xz need the full suffix appended.
    return QDir(m_destDir).filePath(m_nameEdit->text() + QLatin1Char('.') + ext);
}

QString CompressDialog::passphrase() const {
    const QString p = m_passphraseEdit->text();
    if (p.size() < kMinPassphraseLength)
        return {};
    return p;
}

bool CompressDialog::encryptHeaders() const {
    return m_encryptHeadersCheck->isChecked();
}

int CompressDialog::compressionLevel() const {
    return m_levelSpinner->value();
}
