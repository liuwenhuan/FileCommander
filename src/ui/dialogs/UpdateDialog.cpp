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
    resize(520, 360);

    m_headlineLabel = new QLabel(
        tr("Version %1 is available (you have %2).").arg(info.version, QStringLiteral(TTC_VERSION)), this);
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
    m_notesEdit->setPlainText(info.notes.isEmpty() ? tr("No release notes provided.") : info.notes);

    auto *howTo = new QLabel(tr("Visit the update page to download and install the package yourself:"), this);
    howTo->setWordWrap(true);
    m_pageField = new QLineEdit(UpdateChecker::updatePageUrl(), this);
    m_pageField->setObjectName(QStringLiteral("UpdatePageUrl"));
    m_pageField->setReadOnly(true);
    m_pageField->setCursorPosition(0);

    m_closeButton = new QPushButton(tr("Close"), this);
    m_openButton = new QPushButton(tr("Open Update Page"), this);
    m_openButton->setObjectName(QStringLiteral("UpdatePageButton"));
    m_openButton->setDefault(true);
    auto *buttonRow = new QHBoxLayout;
    buttonRow->addStretch();
    buttonRow->addWidget(m_closeButton);
    buttonRow->addWidget(m_openButton);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_headlineLabel);
    layout->addWidget(m_dateLabel);
    layout->addWidget(notesTitle);
    layout->addWidget(m_notesEdit, 1);
    layout->addWidget(howTo);
    layout->addWidget(m_pageField);
    layout->addLayout(buttonRow);

    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_openButton, &QPushButton::clicked, this, &UpdateDialog::openUpdatePage);
}

void UpdateDialog::openUpdatePage() {
    QDesktopServices::openUrl(QUrl(UpdateChecker::updatePageUrl()));
}

QString UpdateDialog::updatePageText() const {
    return m_pageField ? m_pageField->text() : QString();
}
