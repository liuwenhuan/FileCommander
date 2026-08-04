#pragma once

#include <QDialogButtonBox>
#include <QFontDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QString>

class QAbstractButton;
class QDialog;
class QLabel;
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

// The window message() shows, without the modal loop: a themed frameless dialog
// sized to a real QMessageBox embedded in it as a plain child widget. The caller
// owns the returned dialog (or `parent` does, if given).
//
// Split out because that sizing is the part worth testing, and it cannot be
// observed through message(): measuring it means reaching the dialog before
// exec() returns, and showing a QMessageBox at all is not something every QPA
// plugin survives (QMessageBox::showEvent dereferences the platform's native
// interface on Windows, which the offscreen plugin does not provide).
QDialog *createMessageDialog(QWidget *parent, QMessageBox::Icon icon, const QString &title,
                             const QString &text,
                             QMessageBox::StandardButtons buttons = QMessageBox::Ok,
                             QMessageBox::StandardButton defaultButton = QMessageBox::NoButton);

// Stops a label whose text can be arbitrarily long — a file path, an error
// message, a server name, a line of command output — from deciding how wide its
// window is.
//
// A QLabel reports its whole text as its minimum width (its longest unbreakable
// run, once wrapped), and QLayout hands that on as the window's minimum width.
// The window is then born as wide as the string and cannot be shrunk
// afterwards: not a size anyone chose, and on a long path not one that fits the
// screen. Wrapping the text and letting the label be squeezed below its own hint
// moves the decision back to the window, which is the only place that knows how
// wide a dialog should be. Callers still have to give the window a width of its
// own — a resize(), or a layout that supplies one.
void relaxLabelWidth(QLabel *label);

// Applies Qt's translated standard-button labels, with an application fallback
// for a missing or empty qtbase catalog. The installed event filter repeats the
// operation after QEvent::LanguageChange, so already-open dialogs stay current.
void localizeStandardButtons(QMessageBox *box);
void localizeStandardButtons(QDialogButtonBox *box);

// Marks a standard button as an application-owned label. The supplied text is
// restored after every language change instead of being replaced by Qt or the
// fallback catalog; use this when an intentional label equals Qt's source text.
void setStandardButtonOverride(QAbstractButton *button, const QString &text);

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

QFont getFont(bool *ok, const QFont &initial, QWidget *parent = nullptr,
              const QString &title = QString(),
              QFontDialog::FontDialogOptions options = QFontDialog::FontDialogOptions());

} // namespace ttc
