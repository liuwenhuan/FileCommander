#include "OverwriteConfirmDialog.h"
#include "ThemedDialogs.h"

#include <QDialogButtonBox>
#include <QFileInfo>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

// "1.2 MB", or "unknown size" for the -1 that means the operation could not
// find out. Never a bare number for an unknown: the whole point of this prompt
// is that the two sizes are what the user compares.
QString sizeText(qint64 bytes) {
    if (bytes < 0)
        return QObject::tr("unknown size");
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double size = static_cast<double>(bytes);
    int unit = 0;
    while (size >= 1024.0 && unit < 4) {
        size /= 1024.0;
        ++unit;
    }
    if (unit == 0)
        return QStringLiteral("%1 B").arg(bytes);
    // Exact byte count alongside the readable one: overwrite decisions turn on
    // small differences, and "1.2 MB" versus "1.2 MB" hides them. No brackets
    // here -- the caller already parenthesises this.
    return QStringLiteral("%1 %2, %3 bytes").arg(size, 0, 'f', 1).arg(units[unit]).arg(bytes);
}

} // namespace

QString OverwriteConfirmDialog::describe(const FileConflict &conflict) {
    // The destination's file name, taken from the path as a string: the path may
    // be the server's, so QFileInfo must not be asked anything about it beyond
    // splitting it -- and QFileInfo::fileName() is pure string work.
    const QString destName = QFileInfo(conflict.destPath).fileName();
    return QObject::tr("%1 already exists.\n\nSource: %2 (%3)\nDestination: %4 (%5)")
        .arg(destName, conflict.sourcePath, sizeText(conflict.sourceSize), conflict.destPath,
             sizeText(conflict.destSize));
}

OverwriteConfirmDialog::OverwriteConfirmDialog(const FileConflict &conflict, QWidget *parent)
    : FramelessDialog(parent) {
    setWindowTitle(tr("Confirm Overwrite"));
    setModal(true);

    auto *message = new QLabel(describe(conflict), this);
    message->setWordWrap(true);

    auto *buttons = new QDialogButtonBox(this);
    auto addAction = [&](const QString &text, ErrorAction action) {
        QPushButton *btn = buttons->addButton(text, QDialogButtonBox::ActionRole);
        connect(btn, &QPushButton::clicked, this, [this, action]() {
            m_result = action;
            accept();
        });
    };

    addAction(tr("Overwrite"), ErrorAction::Overwrite);
    addAction(tr("Overwrite All"), ErrorAction::OverwriteAll);
    addAction(tr("Skip"), ErrorAction::Skip);
    addAction(tr("Skip All"), ErrorAction::SkipAll);
    addAction(tr("Rename"), ErrorAction::Rename);
    QPushButton *cancelBtn = buttons->addButton(QDialogButtonBox::Cancel);
    ttc::localizeStandardButtons(buttons);
    connect(cancelBtn, &QPushButton::clicked, this, [this]() {
        m_result = ErrorAction::Cancel;
        reject();
    });

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(message);
    layout->addWidget(buttons);
}

ErrorAction OverwriteConfirmDialog::ask(QWidget *parent, const FileConflict &conflict) {
    OverwriteConfirmDialog dlg(conflict, parent);
    dlg.exec();
    return dlg.m_result;
}
