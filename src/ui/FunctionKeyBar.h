#pragma once

#include <QWidget>

class QPushButton;

// Bottom bar of F3-F8 buttons mirroring Total Commander's function-key row.
// Each button is a reassignable slot: left-click runs its function, and the
// right-click menu lets the user change which function it runs. MainWindow
// owns the slot->command mapping and sets the labels.
class FunctionKeyBar : public QWidget {
    Q_OBJECT

public:
    explicit FunctionKeyBar(QWidget *parent = nullptr);

    static constexpr int Count = 6; // F3..F8

    void setLabel(int index, const QString &text);

signals:
    void activated(int index);       // button clicked
    void changeRequested(int index); // "change function" chosen from the menu

private:
    QPushButton *m_buttons[Count];
};
