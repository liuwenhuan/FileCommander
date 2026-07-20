#include "CommandBar.h"

#include <QDir>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>

CommandBar::CommandBar(QWidget *parent) : QWidget(parent) {
    m_prompt = new QLabel(this);
    m_prompt->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_input = new QLineEdit(this);
    m_input->setPlaceholderText(tr("Run a command in the current directory…"));
    m_input->installEventFilter(this);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(2, 0, 2, 0);
    layout->setSpacing(4);
    layout->addWidget(m_prompt);
    layout->addWidget(m_input, 1);

    connect(m_input, &QLineEdit::returnPressed, this, &CommandBar::submit);
}

void CommandBar::setDirectory(const QString &dir) {
    m_directory = dir;
    // Show the full path as the prompt so it's clear where a command will run.
    m_prompt->setText(QStringLiteral("%1 $").arg(dir));
    m_prompt->setToolTip(dir);
}

void CommandBar::focusInput() {
    m_input->setFocus();
}

void CommandBar::submit() {
    const QString command = m_input->text().trimmed();
    if (command.isEmpty())
        return;
    m_history.add(command);
    m_input->clear();
    emit commandSubmitted(command, m_directory);
}

bool CommandBar::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_input && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        if (ke->key() == Qt::Key_Up) {
            m_input->setText(m_history.older(m_input->text()));
            return true;
        }
        if (ke->key() == Qt::Key_Down) {
            m_input->setText(m_history.newer());
            return true;
        }
        if (ke->key() == Qt::Key_Escape) {
            m_input->clear();
            m_history.resetCursor();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}
