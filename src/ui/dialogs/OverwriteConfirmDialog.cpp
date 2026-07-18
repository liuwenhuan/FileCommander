#include "OverwriteConfirmDialog.h"

#include <QDialogButtonBox>
#include <QFileInfo>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

OverwriteConfirmDialog::OverwriteConfirmDialog(const QString &source, const QString &destination,
                                                 QWidget *parent)
    : QDialog(parent) {
    setWindowTitle(tr("Confirm Overwrite"));
    setModal(true);

    QFileInfo srcInfo(source);
    QFileInfo destInfo(destination);

    auto *message = new QLabel(
        tr("%1 already exists.\n\nSource: %2 (%3 bytes)\nDestination: %4 (%5 bytes)")
            .arg(destInfo.fileName(), source)
            .arg(srcInfo.size())
            .arg(destination)
            .arg(destInfo.size()),
        this);
    message->setWordWrap(true);

    auto *buttons = new QDialogButtonBox(this);
    auto addAction = [&](const QString &text, ErrorAction action) {
        QPushButton *btn = buttons->addButton(text, QDialogButtonBox::ActionRole);
        connect(btn, &QPushButton::clicked, this, [this, action]() {
            m_result = action;
            accept();
        });
    };

    addAction(tr("Overwrite"), ErrorAction::Overwrite);
    addAction(tr("Overwrite All"), ErrorAction::OverwriteAll);
    addAction(tr("Skip"), ErrorAction::Skip);
    addAction(tr("Skip All"), ErrorAction::SkipAll);
    addAction(tr("Rename"), ErrorAction::Rename);
    QPushButton *cancelBtn = buttons->addButton(QDialogButtonBox::Cancel);
    connect(cancelBtn, &QPushButton::clicked, this, [this]() {
        m_result = ErrorAction::Cancel;
        reject();
    });

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(message);
    layout->addWidget(buttons);
}

ErrorAction OverwriteConfirmDialog::ask(QWidget *parent, const QString &source,
                                         const QString &destination) {
    OverwriteConfirmDialog dlg(source, destination, parent);
    dlg.exec();
    return dlg.m_result;
}
