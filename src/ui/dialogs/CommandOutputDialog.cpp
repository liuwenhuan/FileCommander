#include "CommandOutputDialog.h"
#include "ThemedDialogs.h"

#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QVBoxLayout>

CommandOutputDialog::CommandOutputDialog(QWidget *parent) : FramelessDialog(parent) {
    setWindowTitle(tr("Command Output"));
    // Non-modal: the user keeps working in the file panels while a command runs.
    setModal(false);
    resize(720, 380);

    m_output = new QPlainTextEdit(this);
    m_output->setReadOnly(true);
    m_output->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    // A generous scrollback for long listings; older lines drop off the top.
    m_output->setMaximumBlockCount(5000);

    auto *buttons = new QDialogButtonBox(this);
    QPushButton *clearButton = buttons->addButton(tr("Clear"), QDialogButtonBox::ResetRole);
    buttons->addButton(QDialogButtonBox::Close);
    ttc::localizeStandardButtons(buttons);
    connect(clearButton, &QPushButton::clicked, m_output, &QPlainTextEdit::clear);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::hide);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_output, 1);
    layout->addWidget(buttons);
}

void CommandOutputDialog::beginCommand(const QString &command, const QString &cwd) {
    if (!m_output->document()->isEmpty())
        m_output->appendPlainText(QString()); // blank line between commands
    m_output->appendPlainText(QStringLiteral("%1 $ %2").arg(cwd, command));
    // Start command output on its own line: appendOutput() inserts at the end of
    // the current block, so without this the first chunk of stdout is glued to the
    // end of the header (".. $ cmd" + "output" on one line).
    QTextCursor cursor = m_output->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(QStringLiteral("\n"));
}

void CommandOutputDialog::appendOutput(const QString &text) {
    if (text.isEmpty())
        return;
    // insertPlainText (not appendPlainText) so partial lines from a chunked read
    // don't each become their own paragraph; trailing newlines are preserved.
    QTextCursor cursor = m_output->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text);
    m_output->verticalScrollBar()->setValue(m_output->verticalScrollBar()->maximum());
    reveal();
}

void CommandOutputDialog::endCommand(int exitCode, bool crashed) {
    if (crashed) {
        m_output->appendPlainText(tr("[command failed to run or crashed]"));
        reveal();
    } else if (exitCode != 0) {
        m_output->appendPlainText(tr("[exited with code %1]").arg(exitCode));
        reveal();
    }
    m_output->verticalScrollBar()->setValue(m_output->verticalScrollBar()->maximum());
}

void CommandOutputDialog::reveal() {
    if (isHidden())
        show();
    raise();
}

void CommandOutputDialog::retranslate() {
    setWindowTitle(tr("Command Output"));
}
