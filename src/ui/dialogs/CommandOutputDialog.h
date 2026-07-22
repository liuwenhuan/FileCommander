#pragma once

#include <QDialog>

class QPlainTextEdit;

// Non-modal console that shows the output of commands run from the command bar
// ("run a command in the current directory"). Previously commands were launched
// detached, so anything that only printed to stdout/stderr (ls, echo, grep, a
// failing command's error) appeared to do nothing. This captures and displays
// that output so the feature is visibly useful.
//
// It shows itself only when there is something worth seeing (any output, or a
// non-zero / crashed exit) so pure side-effecting commands (mkdir, touch) stay
// out of the way and just refresh the panels as before.
class CommandOutputDialog : public QDialog {
    Q_OBJECT

public:
    explicit CommandOutputDialog(QWidget *parent = nullptr);

    // Prints a "cwd $ command" header for a newly started command. Does not show
    // the window on its own -- output/exit drive visibility.
    void beginCommand(const QString &command, const QString &cwd);
    // Appends captured output (stdout+stderr, interleaved) and reveals the window.
    void appendOutput(const QString &text);
    // Prints the exit status; reveals the window on failure so errors aren't missed.
    void endCommand(int exitCode, bool crashed);

    void retranslate();

private:
    void reveal(); // show + raise without stealing the whole app's activation

    QPlainTextEdit *m_output;
};
