#pragma once

#include <QWidget>

// Bottom bar of F3-F8 buttons mirroring Total Commander's function-key row.
// Clicking a button fires the same signal MainWindow's key shortcuts use.
class FunctionKeyBar : public QWidget {
    Q_OBJECT

public:
    explicit FunctionKeyBar(QWidget *parent = nullptr);

signals:
    void viewRequested();   // F3
    void editRequested();   // F4
    void copyRequested();   // F5
    void moveRequested();   // F6
    void mkdirRequested();  // F7
    void deleteRequested(); // F8
};
