#include "AboutDialog.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "version.h"

AboutDialog::AboutDialog(const QIcon &icon, QWidget *parent) : FramelessDialog(parent) {
    setWindowTitle(tr("About FileCommander"));
    setModal(true);

    // Icon: prefer the caller-supplied one, fall back to the window icon.
    auto *iconLabel = new QLabel(this);
    const QIcon effectiveIcon = icon.isNull() ? windowIcon() : icon;
    iconLabel->setPixmap(effectiveIcon.pixmap(64, 64));
    iconLabel->setAlignment(Qt::AlignCenter);

    auto *nameLabel = new QLabel(tr("FileCommander — Total Commander for Linux"), this);
    nameLabel->setAlignment(Qt::AlignCenter);
    QFont nameFont = nameLabel->font();
    nameFont.setPointSizeF(nameFont.pointSizeF() * 1.3);
    nameFont.setBold(true);
    nameLabel->setFont(nameFont);

    auto *versionLabel = new QLabel(tr("Version %1").arg(QStringLiteral(TTC_VERSION)), this);
    versionLabel->setAlignment(Qt::AlignCenter);

    auto *descLabel = new QLabel(
        tr("FileCommander is a powerful dual-pane file manager inspired by Total Commander and "
           "developed from scratch. It manages local files efficiently, makes network file "
           "services convenient to use, previews many file types, and is an efficiency tool "
           "built for advanced users."),
        this);
    descLabel->setAlignment(Qt::AlignCenter);
    descLabel->setWordWrap(true);

    auto *copyrightLabel = new QLabel(
        tr("Free and open-source software, released under the GNU GPL v3."), this);
    copyrightLabel->setAlignment(Qt::AlignCenter);
    copyrightLabel->setWordWrap(true);

    auto *linkLabel = new QLabel(
        tr("<a href=\"https://github.com/ttc-fm/ttc\">Project home page</a>"), this);
    linkLabel->setAlignment(Qt::AlignCenter);
    linkLabel->setOpenExternalLinks(true);
    linkLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);

    auto *closeButton = new QPushButton(tr("Close"), this);
    closeButton->setDefault(true);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->addStretch();
    buttonRow->addWidget(closeButton);
    buttonRow->addStretch();

    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(10);
    layout->addStretch();
    layout->addWidget(iconLabel);
    layout->addWidget(nameLabel);
    layout->addWidget(versionLabel);
    layout->addSpacing(6);
    layout->addWidget(descLabel);
    layout->addWidget(copyrightLabel);
    layout->addWidget(linkLabel);
    layout->addStretch();
    layout->addLayout(buttonRow);

    setMinimumWidth(360);
}
