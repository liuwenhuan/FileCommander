#pragma once

#include <QWidget>

class QMouseEvent;
class QPaintEvent;

// A slim self-drawn title bar for frameless dialogs. Mirrors the main window's
// TitleBar chrome (theme-following background, rounded top corners, centred
// dimmed title, a self-painted close button) but without the menu bar or the
// minimize / maximize buttons — dialogs only close. The title text is read live
// from the host window's windowTitle().
class DialogTitleBar : public QWidget {
    Q_OBJECT
public:
    explicit DialogTitleBar(QWidget *window, QWidget *parent = nullptr);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    QWidget *m_window = nullptr;
    bool m_pressed = false;
};
