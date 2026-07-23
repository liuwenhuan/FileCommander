#pragma once

#include <QWidget>

class QPushButton;
class QIcon;

// Bottom bar of F3-F8 buttons mirroring Total Commander's function-key row.
// Each button is a reassignable slot: left-click runs its function, and the
// right-click menu lets the user change which function it runs. MainWindow
// owns the slot->command mapping and sets the labels.
//
// Flanking the F-key row are two extra square, icon-only buttons (a "leading"
// one before F3 and a "trailing" one after F8). They follow the same
// reassignable-command model but are keyed by slot name; MainWindow defaults
// them to the external-connection and quick-notepad commands.
class FunctionKeyBar : public QWidget {
    Q_OBJECT

public:
    explicit FunctionKeyBar(QWidget *parent = nullptr);

    static constexpr int Count = 6; // F3..F8

    void setLabel(int index, const QString &text);

    void setLeadingIcon(const QIcon &icon);
    void setTrailingIcon(const QIcon &icon);
    void setLeadingToolTip(const QString &text);
    void setTrailingToolTip(const QString &text);

    // Global-screen geometry of the leading / trailing square buttons, used to
    // anchor a floating popup directly above the button that launched it.
    QRect leadingButtonGlobalRect() const;
    QRect trailingButtonGlobalRect() const;

signals:
    void activated(int index);       // button clicked
    void changeRequested(int index); // "change function" chosen from the menu

    void leadingActivated();
    void leadingChangeRequested();
    void trailingActivated();
    void trailingChangeRequested();

private:
    QPushButton *m_leadingButton;
    QPushButton *m_buttons[Count];
    QPushButton *m_trailingButton;
};
