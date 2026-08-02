#pragma once

#include <QWidget>

#include "CommandHistory.h"

class QLabel;
class QLineEdit;

// A shell-style command line pinned to the bottom of the window. Shows the
// active directory as a prompt, runs the typed command there on Enter, and
// keeps an Up/Down history. It only emits commandSubmitted(); MainWindow owns
// the actual process launch and knows the current directory.
class CommandBar : public QWidget {
    Q_OBJECT

public:
    explicit CommandBar(QWidget *parent = nullptr);

    void setDirectory(const QString &dir);
    void focusInput();
    // Appends text to the input (space-separated from whatever is already
    // typed) and focuses it, for the "put this path on the command line"
    // shortcuts. Quotes the text when it contains spaces so the result is
    // directly runnable.
    void appendText(const QString &text);
    // Re-applies translated text (the input placeholder) after a language change.
    void retranslate();

signals:
    void commandSubmitted(const QString &command, const QString &directory);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void submit();

    QLabel *m_prompt;
    QLineEdit *m_input;
    QString m_directory;
    CommandHistory m_history;
};
