#pragma once

#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QStackedWidget;

// Lightweight in-panel preview shown by Ctrl+Q: renders the file under the
// cursor as scaled image, a text head, or a "no preview" note.
class QuickView : public QWidget {
    Q_OBJECT

public:
    explicit QuickView(QWidget *parent = nullptr);

    void showFile(const QString &path);

private:
    QStackedWidget *m_stack;
    QLabel *m_image;
    QPlainTextEdit *m_text;
    QLabel *m_info;
};
