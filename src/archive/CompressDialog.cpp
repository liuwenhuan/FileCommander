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

bool formatSupportsZipOptions(const QString &format) {
    return format == QLatin1String("zip") ||
           format == QLatin1String("7z")  ||
           format == QLatin1String("tar.bz2") ||
           format == QLatin1String("tar.xz");
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

    // --- ZIP-option block: passphrase + header encryption + compression level ---
    m_zipOptions = new QWidget(this);
    auto *zipLayout = new QFormLayout(m_zipOptions);
    zipLayout->setContentsMargins(0, 4, 0, 0);

    m_passphraseEdit = new QLineEdit(m_zipOptions);
    m_passphraseEdit->setEchoMode(QLineEdit::Password);
    m_passphraseEdit->setPlaceholderText(tr("Leave empty for no encryption"));
    zipLayout->addRow(tr("Password:"), m_passphraseEdit);

    m_encryptHeadersCheck = new QCheckBox(tr("Encrypt file list too"), m_zipOptions);
    m_encryptHeadersCheck->setToolTip(
        tr("When checked, individual file names inside the archive are encrypted. "
           "Uncheck to see the file list without the password (ZIP-style)."));
    // Only applicable to 7z header encryption.
    m_encryptHeadersCheck->setChecked(false);
    zipLayout->addRow(m_encryptHeadersCheck);

    // Shared label + spinner for compression level.
    auto *levelRow = new QHBoxLayout;
    m_levelLabel = new QLabel(tr("Compression level:"), m_zipOptions);
    m_levelSpinner = new QSpinBox(m_zipOptions);
    m_levelSpinner->setRange(0, 9);
    m_levelSpinner->setValue(kDefaultLevel);
    m_levelSpinner->setToolTip(tr("0 = store only, 9 = best compression"));
    levelRow->addWidget(m_levelLabel);
    levelRow->addStretch(1);
    levelRow->addWidget(m_levelSpinner);
    zipLayout->addRow(levelRow);

    // --- Main form -----------------------------------------------
    auto *form = new QFormLayout;
    form->addRow(tr("Archive name:"), m_nameEdit);
    form->addRow(tr("Format:"), m_formatCombo);
    form->addRow(m_zipOptions);

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
    const bool showExtra = formatSupportsZipOptions(fmt);

    m_zipOptions->setVisible(showExtra);
    m_encryptHeadersCheck->setVisible(fmt == QLatin1String("7z"));

    const bool showLevel = formatSupportsLevel(fmt);
    m_levelLabel->setVisible(showLevel);
    m_levelSpinner->setVisible(showLevel);
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
