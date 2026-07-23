#pragma once

#include <QLineEdit>
#include <QMessageBox>
#include <QString>

class QWidget;

// Drop-in themed replacements for the QMessageBox / QInputDialog static
// convenience functions. They embed a real QMessageBox / QInputDialog as a
// child widget inside a FramelessDialog, so the message layout, icons, standard
// buttons, and return values behave exactly like the originals — but the window
// wears the app's self-drawn frameless chrome (themed, taller title bar) instead
// of the native decorations (which the WM renders short and light).
//
// Signatures mirror the Qt statics so call sites change only their prefix
// (QMessageBox:: -> ttc::, QInputDialog:: -> ttc::).
namespace ttc {

QMessageBox::StandardButton
message(QWidget *parent, QMessageBox::Icon icon, const QString &title, const QString &text,
        QMessageBox::StandardButtons buttons = QMessageBox::Ok,
        QMessageBox::StandardButton defaultButton = QMessageBox::NoButton);

inline QMessageBox::StandardButton
information(QWidget *parent, const QString &title, const QString &text,
           QMessageBox::StandardButtons buttons = QMessageBox::Ok,
           QMessageBox::StandardButton defaultButton = QMessageBox::NoButton) {
    return message(parent, QMessageBox::Information, title, text, buttons, defaultButton);
}

inline QMessageBox::StandardButton
warning(QWidget *parent, const QString &title, const QString &text,
        QMessageBox::StandardButtons buttons = QMessageBox::Ok,
        QMessageBox::StandardButton defaultButton = QMessageBox::NoButton) {
    return message(parent, QMessageBox::Warning, title, text, buttons, defaultButton);
}

inline QMessageBox::StandardButton
critical(QWidget *parent, const QString &title, const QString &text,
         QMessageBox::StandardButtons buttons = QMessageBox::Ok,
         QMessageBox::StandardButton defaultButton = QMessageBox::NoButton) {
    return message(parent, QMessageBox::Critical, title, text, buttons, defaultButton);
}

inline QMessageBox::StandardButton
question(QWidget *parent, const QString &title, const QString &text,
         QMessageBox::StandardButtons buttons = QMessageBox::StandardButtons(QMessageBox::Yes |
                                                                             QMessageBox::No),
         QMessageBox::StandardButton defaultButton = QMessageBox::NoButton) {
    return message(parent, QMessageBox::Question, title, text, buttons, defaultButton);
}

QString getText(QWidget *parent, const QString &title, const QString &label,
                QLineEdit::EchoMode mode = QLineEdit::Normal, const QString &text = QString(),
                bool *ok = nullptr);

int getInt(QWidget *parent, const QString &title, const QString &label, int value = 0,
           int min = -2147483647, int max = 2147483647, int step = 1, bool *ok = nullptr);

} // namespace ttc
