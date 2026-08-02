#include "OperationErrorDialog.h"

#include <QEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayoutItem>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QShowEvent>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>

namespace {

QLabel *detailLabel(const QString &text, const QString &objectName, const QString &semanticState,
                    QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(objectName);
    label->setWordWrap(true);
    label->setMinimumWidth(0);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    label->setProperty("semanticState", semanticState);
    return label;
}

} // namespace

OperationErrorDialog::OperationErrorDialog(const OperationError &error, bool elevationAvailable,
                                           QWidget *parent)
    : FramelessDialog(parent)
{
    setWindowTitle(tr("File Operation Failed"));
    setModal(true);
    // Application-modal only blocks input to sibling windows; it does not guarantee this
    // window paints above them. The transfer progress window keeps repainting (ETA/speed
    // ticks, translucent-background layered-window updates) while this dialog's own nested
    // event loop is running, which can otherwise re-cover this dialog after the initial
    // raise(). Stays-on-top makes the stacking unconditional for this short-lived prompt.
    setWindowFlag(Qt::WindowStaysOnTopHint, true);

    auto *details = new QWidget(this);
    auto *detailsLayout = new QVBoxLayout(details);
    detailsLayout->setContentsMargins(0, 0, 0, 0);

    const QString reason = error.message.isEmpty() ? operationErrorFallbackMessage(error.category)
                                                    : error.message;
    detailsLayout->addWidget(detailLabel(reason, QStringLiteral("OperationErrorReason"),
                                         QStringLiteral("error"), details));
    detailsLayout->addWidget(detailLabel(tr("Source: %1").arg(error.sourcePath),
                                         QStringLiteral("OperationErrorSource"),
                                         QStringLiteral("muted"), details));
    detailsLayout->addWidget(detailLabel(tr("Target: %1").arg(error.targetPath),
                                         QStringLiteral("OperationErrorTarget"),
                                         QStringLiteral("muted"), details));
    // Without this, three "Preferred" (not "Fixed") vertical-policy labels each grow to
    // fill their share of any space the scroll area is given beyond their own wrapped-text
    // height -- visible as a huge gap after every single line -- instead of that slack
    // collecting harmlessly below the last line, which is what this stretch is for.
    detailsLayout->addStretch(1);

    auto *detailsScroll = new QScrollArea(this);
    detailsScroll->setFrameShape(QFrame::NoFrame);
    detailsScroll->setWidgetResizable(true);
    detailsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    detailsScroll->setWidget(details);

    m_actionPanel = new QWidget(this);
    m_actionLayout = new QVBoxLayout(m_actionPanel);
    m_actionLayout->setContentsMargins(0, 0, 0, 0);

    addAction(tr("Retry"), ErrorAction::Retry, QStringLiteral("OperationErrorRetry"));
    addAction(tr("Skip"), ErrorAction::Skip, QStringLiteral("OperationErrorSkip"));
    addAction(tr("Skip All"), ErrorAction::SkipAll, QStringLiteral("OperationErrorSkipAll"));
    addAction(tr("Cancel Current"), ErrorAction::Cancel,
              QStringLiteral("OperationErrorCancelCurrent"));
    addAction(tr("Abort"), ErrorAction::Abort, QStringLiteral("OperationErrorAbort"));
    if (!error.remote && error.elevatable && elevationAvailable) {
        addAction(tr("Run as administrator"), ErrorAction::Elevate,
                  QStringLiteral("OperationErrorElevate"));
    }
    updateActionLayout();

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(detailsScroll, 1);
    layout->addWidget(m_actionPanel);

    QScreen *dialogScreen = screen();
    if (!dialogScreen)
        dialogScreen = QGuiApplication::primaryScreen();
    if (dialogScreen) {
        const QRect available = dialogScreen->availableGeometry();
        // A modal decision prompt with a few lines of text and a couple of button rows has
        // no business filling most of the screen -- cap it well below the screen size
        // regardless of what the content would otherwise be handed.
        const QSize cap(available.width() * 2 / 3, available.height() * 2 / 3);
        setMaximumSize(cap);
        // Fix the width first and force the layout to run immediately -- rather than wait
        // for the deferred pass in resizeEvent/showEvent -- so the wrapped labels get a
        // real width to compute heightForWidth against before anything below reads a size
        // that depends on it.
        resize(qMin(640, cap.width()), height());
        layout->activate();

        // detailsScroll (stretch=1 above) will otherwise claim any leftover height the
        // window ends up with beyond what the layout below actually needs -- e.g. the
        // window is capped to the *screen's* height, not this dialog's natural content
        // height, and nothing else in a QVBoxLayout consumes unclaimed stretch space.
        // Capping it to what "details" (now correctly measured against a real width, thanks
        // to the resize+activate() above) actually needs turns that would-be gap under the
        // text into nothing rather than a blank scroll area, regardless of how tall the
        // window itself ends up.
        details->adjustSize();
        detailsScroll->setMaximumHeight(details->height() + 2 * detailsScroll->frameWidth());

        resize(width(), qMin(sizeHint().height(), available.height()));

        // Centered over the parent (or the screen), clamped so it can never land partially
        // off-screen.
        QRect target(QPoint(0, 0), size());
        if (QWidget *owner = parentWidget())
            target.moveCenter(owner->frameGeometry().center());
        else
            target.moveCenter(available.center());
        if (target.right() > available.right())
            target.moveRight(available.right());
        if (target.bottom() > available.bottom())
            target.moveBottom(available.bottom());
        if (target.left() < available.left())
            target.moveLeft(available.left());
        if (target.top() < available.top())
            target.moveTop(available.top());
        move(target.topLeft());
    }
}

void OperationErrorDialog::addAction(const QString &text, ErrorAction action,
                                     const QString &objectName)
{
    auto *button = new QPushButton(text, m_actionPanel);
    button->setObjectName(objectName);
    button->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    connect(button, &QPushButton::clicked, this, [this, action] {
        m_result = action;
        accept();
    });
    m_actionButtons.append(button);
}

void OperationErrorDialog::updateActionLayout()
{
    if (!m_actionPanel || !m_actionLayout)
        return;

    while (QLayoutItem *item = m_actionLayout->takeAt(0)) {
        if (QLayout *row = item->layout()) {
            while (QLayoutItem *buttonItem = row->takeAt(0))
                delete buttonItem; // drops the wrapper only; the button widget lives on
        }
        delete item;
    }

    if (m_actionButtons.isEmpty())
        return;

    // Two fixed rows, split evenly, rather than width-based wrapping: a stable, predictable
    // layout the user can learn once, instead of row membership (and row count) shifting
    // with dialog width or font size. This also sidesteps requiring a real width to arrange
    // buttons at all, so this can safely run before the dialog has ever been shown/sized.
    const int firstRowCount = (m_actionButtons.size() + 1) / 2;
    auto addRow = [this](const QList<QPushButton *> &buttons) {
        auto *row = new QHBoxLayout();
        row->setContentsMargins(0, 0, 0, 0);
        for (QPushButton *button : buttons)
            row->addWidget(button);
        row->addStretch(1); // keeps buttons packed left/natural-width instead of stretching
        m_actionLayout->addLayout(row);
    };
    addRow(m_actionButtons.mid(0, firstRowCount));
    if (firstRowCount < m_actionButtons.size())
        addRow(m_actionButtons.mid(firstRowCount));
}

void OperationErrorDialog::changeEvent(QEvent *event)
{
    FramelessDialog::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::ApplicationFontChange ||
        event->type() == QEvent::StyleChange) {
        QTimer::singleShot(0, this, [this] { updateActionLayout(); });
    }
}

void OperationErrorDialog::resizeEvent(QResizeEvent *event)
{
    FramelessDialog::resizeEvent(event);
    QTimer::singleShot(0, this, [this] { updateActionLayout(); });
}

void OperationErrorDialog::showEvent(QShowEvent *event)
{
    FramelessDialog::showEvent(event);
    QTimer::singleShot(0, this, [this] { updateActionLayout(); });
    // Being application-modal blocks input to sibling top-level windows, but it does not
    // guarantee this window paints above them (e.g. the non-modal transfer progress
    // window can otherwise still cover it). Force the stacking explicitly.
    raise();
    activateWindow();
}

ErrorAction OperationErrorDialog::ask(QWidget *parent, const OperationError &error,
                                      bool elevationAvailable)
{
    OperationErrorDialog dialog(error, elevationAvailable, parent);
    dialog.exec();
    return dialog.m_result;
}
