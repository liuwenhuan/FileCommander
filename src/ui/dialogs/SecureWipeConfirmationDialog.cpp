#include "SecureWipeConfirmationDialog.h"

#include "ThemedDialogs.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextDocument>
#include <QVBoxLayout>

#include <cmath>

namespace {

constexpr int kVisiblePathLines = 4;

QStringList absolutePaths(const QStringList &paths) {
    QStringList result;
    result.reserve(paths.size());
    for (const QString &path : paths)
        result.append(QFileInfo(path).absoluteFilePath());
    return result;
}

} // namespace

SecureWipeConfirmationDialog::SecureWipeConfirmationDialog(const QStringList &paths,
                                                           qint64 totalBytes,
                                                           QWidget *parent)
    : FramelessDialog(parent) {
    setWindowTitle(tr("Secure Wipe"));
    setModal(true);
    resize(640, 300);

    auto *summary = new QLabel(
        tr("Securely erase %1 item(s) (%2 bytes)?").arg(paths.size()).arg(totalBytes), this);
    summary->setObjectName(QStringLiteral("SecureWipeSummary"));
    summary->setWordWrap(true);

    auto *warning = new QLabel(
        tr("Their contents will be overwritten on disk and then deleted. This operation "
           "cannot be undone: the items will not enter the Recycle Bin and cannot be "
           "recovered."),
        this);
    warning->setWordWrap(true);

    m_pathList = new QPlainTextEdit(this);
    auto *pathList = m_pathList;
    pathList->setObjectName(QStringLiteral("SecureWipePathList"));
    pathList->setPlainText(absolutePaths(paths).join(QLatin1Char('\n')));
    pathList->setReadOnly(true);
    pathList->setUndoRedoEnabled(false);
    pathList->setLineWrapMode(QPlainTextEdit::NoWrap);
    pathList->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    pathList->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    pathList->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    updatePathListHeight();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Yes | QDialogButtonBox::No, this);
    ttc::localizeStandardButtons(buttons);
    connect(buttons->button(QDialogButtonBox::Yes), &QPushButton::clicked, this,
            &QDialog::accept);
    connect(buttons->button(QDialogButtonBox::No), &QPushButton::clicked, this,
            &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(summary);
    layout->addWidget(warning);
    layout->addWidget(pathList);
    layout->addWidget(buttons);
}

void SecureWipeConfirmationDialog::changeEvent(QEvent *event) {
    FramelessDialog::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::ApplicationFontChange ||
        event->type() == QEvent::StyleChange) {
        updatePathListHeight();
    }
}

void SecureWipeConfirmationDialog::updatePathListHeight() {
    if (!m_pathList)
        return;
    if (m_pathList->font() != font())
        m_pathList->setFont(font());
    const int height = m_pathList->fontMetrics().lineSpacing() * kVisiblePathLines +
                       m_pathList->frameWidth() * 2 +
                       static_cast<int>(
                           std::ceil(m_pathList->document()->documentMargin() * 2));
    m_pathList->setFixedHeight(height);
}

bool SecureWipeConfirmationDialog::ask(QWidget *parent, const QStringList &paths,
                                       qint64 totalBytes) {
    SecureWipeConfirmationDialog dialog(paths, totalBytes, parent);
    return dialog.exec() == QDialog::Accepted;
}
