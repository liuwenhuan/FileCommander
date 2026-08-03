#include "UpdateDialog.h"

#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTextEdit>
#include <QUrl>
#include <QVBoxLayout>

#include "version.h"

UpdateDialog::UpdateDialog(const UpdateInfo &info, QWidget *parent)
    : FramelessDialog(parent), m_info(info) {
    setWindowTitle(tr("Software Update"));
    setModal(true);
    resize(520, 400);

    m_headlineLabel = new QLabel(
        tr("Version %1 is available (you have %2).").arg(info.version, QStringLiteral(TTC_VERSION)),
        this);
    m_headlineLabel->setObjectName(QStringLiteral("UpdateHeadline"));
    m_headlineLabel->setWordWrap(true);
    QFont headlineFont = m_headlineLabel->font();
    headlineFont.setBold(true);
    m_headlineLabel->setFont(headlineFont);

    m_dateLabel = new QLabel(this);
    if (!info.date.isEmpty())
        m_dateLabel->setText(tr("Released: %1").arg(info.date));

    auto *notesTitle = new QLabel(tr("Release notes:"), this);

    m_notesEdit = new QTextEdit(this);
    m_notesEdit->setReadOnly(true);
    m_notesEdit->setPlainText(info.notes.isEmpty() ? tr("No release notes provided.")
                                                   : info.notes);

    auto *howTo = new QLabel(
        info.storeUrl.isEmpty()
            ? tr("Download the package and install it yourself:")
            : tr("Update through the Microsoft Store, or download the package yourself:"),
        this);
    howTo->setWordWrap(true);

    // A read-only field rather than a label: the reason for showing the address
    // at all is that it can be selected and copied, including on a machine
    // where launching a browser from here is not what the user wants.
    m_urlField = new QLineEdit(info.url, this);
    m_urlField->setObjectName(QStringLiteral("UpdateDownloadUrl"));
    m_urlField->setReadOnly(true);
    m_urlField->setCursorPosition(0);

    // The checksum is what the built-in installer used to verify with. Now that
    // fetching the package is the user's own job, handing them the same value
    // is the only way they can make the same check.
    auto *checksumTitle = new QLabel(tr("SHA-256 (check your download against this):"), this);
    checksumTitle->setWordWrap(true);
    m_checksumField = new QLineEdit(info.sha256, this);
    m_checksumField->setObjectName(QStringLiteral("UpdateChecksum"));
    m_checksumField->setReadOnly(true);
    m_checksumField->setCursorPosition(0);

    m_closeButton = new QPushButton(tr("Close"), this);
    m_downloadButton = new QPushButton(tr("Open Download Page"), this);
    m_downloadButton->setObjectName(QStringLiteral("UpdateDownloadButton"));
    m_downloadButton->setDefault(true);
    m_downloadButton->setEnabled(!info.url.isEmpty());

    auto *buttonRow = new QHBoxLayout;
    buttonRow->addStretch();
    buttonRow->addWidget(m_closeButton);
    if (!info.storeUrl.isEmpty()) {
        m_storeButton = new QPushButton(tr("Get from Microsoft Store"), this);
        m_storeButton->setObjectName(QStringLiteral("UpdateStoreButton"));
        buttonRow->addWidget(m_storeButton);
        connect(m_storeButton, &QPushButton::clicked, this, &UpdateDialog::openStorePage);
    }
    buttonRow->addWidget(m_downloadButton);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_headlineLabel);
    layout->addWidget(m_dateLabel);
    layout->addWidget(notesTitle);
    layout->addWidget(m_notesEdit, 1);
    layout->addWidget(howTo);
    layout->addWidget(m_urlField);
    layout->addWidget(checksumTitle);
    layout->addWidget(m_checksumField);
    layout->addLayout(buttonRow);

    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_downloadButton, &QPushButton::clicked, this, &UpdateDialog::openDownloadPage);
}

void UpdateDialog::openDownloadPage() {
    if (m_info.url.isEmpty())
        return;
    QDesktopServices::openUrl(QUrl(m_info.url));
}

void UpdateDialog::openStorePage() {
    if (m_info.storeUrl.isEmpty())
        return;
    QDesktopServices::openUrl(QUrl(m_info.storeUrl));
}

QString UpdateDialog::downloadUrlText() const {
    return m_urlField ? m_urlField->text() : QString();
}

QString UpdateDialog::checksumText() const {
    return m_checksumField ? m_checksumField->text() : QString();
}

bool UpdateDialog::hasStoreButton() const {
    return m_storeButton != nullptr;
}
