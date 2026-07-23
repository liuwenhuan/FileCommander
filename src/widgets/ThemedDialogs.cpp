#include "ThemedDialogs.h"

#include "FramelessDialog.h"

#include <QAbstractButton>
#include <QInputDialog>
#include <QVBoxLayout>

namespace ttc {

QMessageBox::StandardButton message(QWidget *parent, QMessageBox::Icon icon, const QString &title,
                                    const QString &text, QMessageBox::StandardButtons buttons,
                                    QMessageBox::StandardButton defaultButton) {
    FramelessDialog dlg(parent);
    dlg.setWindowTitle(title);

    // A real QMessageBox, but embedded as a plain child widget (Qt::Widget)
    // rather than a top-level window — so it renders its icon / text / standard
    // buttons inside our themed frame instead of its own native decorations.
    auto *box = new QMessageBox(&dlg);
    box->setIcon(icon);
    box->setWindowTitle(title);
    box->setText(text);
    box->setStandardButtons(buttons);
    if (defaultButton != QMessageBox::NoButton)
        box->setDefaultButton(defaultButton);
    box->setWindowFlags(Qt::Widget);

    QMessageBox::StandardButton result = QMessageBox::NoButton;
    QObject::connect(box, &QMessageBox::buttonClicked, &dlg,
                     [&](QAbstractButton *b) { result = box->standardButton(b); });
    QObject::connect(box, &QMessageBox::finished, &dlg, [&](int) { dlg.accept(); });

    auto *layout = new QVBoxLayout(&dlg);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(box);

    dlg.exec();
    return result;
}

QString getText(QWidget *parent, const QString &title, const QString &label,
                QLineEdit::EchoMode mode, const QString &text, bool *ok) {
    FramelessDialog dlg(parent);
    dlg.setWindowTitle(title);

    auto *input = new QInputDialog(&dlg);
    input->setWindowFlags(Qt::Widget);
    input->setInputMode(QInputDialog::TextInput);
    input->setLabelText(label);
    input->setTextEchoMode(mode);
    input->setTextValue(text);

    bool accepted = false;
    QObject::connect(input, &QInputDialog::accepted, &dlg, [&] {
        accepted = true;
        dlg.accept();
    });
    QObject::connect(input, &QInputDialog::rejected, &dlg, [&] { dlg.reject(); });

    auto *layout = new QVBoxLayout(&dlg);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(input);
    // Give short prompts a comfortable minimum width (QInputDialog alone is very
    // narrow).
    dlg.setMinimumWidth(dlg.sizeHint().width() < 360 ? 360 : dlg.sizeHint().width());

    dlg.exec();
    if (ok)
        *ok = accepted;
    return accepted ? input->textValue() : QString();
}

int getInt(QWidget *parent, const QString &title, const QString &label, int value, int min,
           int max, int step, bool *ok) {
    FramelessDialog dlg(parent);
    dlg.setWindowTitle(title);

    auto *input = new QInputDialog(&dlg);
    input->setWindowFlags(Qt::Widget);
    input->setInputMode(QInputDialog::IntInput);
    input->setLabelText(label);
    input->setIntRange(min, max);
    input->setIntStep(step);
    input->setIntValue(value);

    bool accepted = false;
    QObject::connect(input, &QInputDialog::accepted, &dlg, [&] {
        accepted = true;
        dlg.accept();
    });
    QObject::connect(input, &QInputDialog::rejected, &dlg, [&] { dlg.reject(); });

    auto *layout = new QVBoxLayout(&dlg);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(input);
    dlg.setMinimumWidth(dlg.sizeHint().width() < 360 ? 360 : dlg.sizeHint().width());

    dlg.exec();
    if (ok)
        *ok = accepted;
    return accepted ? input->intValue() : value;
}

} // namespace ttc
