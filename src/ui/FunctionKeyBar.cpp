#include "FunctionKeyBar.h"

#include <QHBoxLayout>
#include <QIcon>
#include <QMenu>
#include <QPushButton>
#include <QRect>

FunctionKeyBar::FunctionKeyBar(QWidget *parent) : QWidget(parent) {
    // A plain QWidget subclass does not paint a stylesheet background unless
    // it is told to; without this the CRT theme's scanline texture stops at
    // the Qt-provided widgets and this one stays flat. light.qss/dark.qss
    // declare `background: transparent` for this class so their appearance is
    // unchanged -- it shows the parent, exactly as it did before.
    setAttribute(Qt::WA_StyledBackground, true);
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 2, 4, 2);

    // Builds one of the two square, icon-only flanking buttons: no text, fixed
    // narrow width, left-click emits `activatedSig`, right-click offers a
    // "change function" action that emits `changeSig`.
    auto makeSquare = [this](void (FunctionKeyBar::*activatedSig)(),
                             void (FunctionKeyBar::*changeSig)()) {
        auto *btn = new QPushButton(this);
        btn->setFocusPolicy(Qt::NoFocus); // keep out of the Tab chain
        btn->setFixedWidth(34);
        btn->setIconSize(QSize(18, 18));
        btn->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(btn, &QPushButton::clicked, this, activatedSig);
        connect(btn, &QWidget::customContextMenuRequested, this,
                [this, btn, changeSig](const QPoint &pos) {
                    QMenu menu(this);
                    menu.addAction(tr("Change this button's function..."), this,
                                   [this, changeSig]() { (this->*changeSig)(); });
                    menu.exec(btn->mapToGlobal(pos));
                });
        return btn;
    };

    m_leadingButton = makeSquare(&FunctionKeyBar::leadingActivated,
                                 &FunctionKeyBar::leadingChangeRequested);
    layout->addWidget(m_leadingButton);

    for (int i = 0; i < Count; ++i) {
        auto *btn = new QPushButton(this);
        btn->setFocusPolicy(Qt::NoFocus); // keep out of the Tab chain
        btn->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(btn, &QPushButton::clicked, this, [this, i]() { emit activated(i); });
        connect(btn, &QWidget::customContextMenuRequested, this,
                [this, i, btn](const QPoint &pos) {
                    QMenu menu(this);
                    menu.addAction(tr("Change this key's function..."), this,
                                   [this, i]() { emit changeRequested(i); });
                    menu.exec(btn->mapToGlobal(pos));
                });
        m_buttons[i] = btn;
        layout->addWidget(btn);
    }

    m_trailingButton = makeSquare(&FunctionKeyBar::trailingActivated,
                                  &FunctionKeyBar::trailingChangeRequested);
    layout->addWidget(m_trailingButton);
}

void FunctionKeyBar::setLabel(int index, const QString &text) {
    if (index >= 0 && index < Count)
        m_buttons[index]->setText(text);
}

void FunctionKeyBar::setLeadingIcon(const QIcon &icon) { m_leadingButton->setIcon(icon); }
void FunctionKeyBar::setTrailingIcon(const QIcon &icon) { m_trailingButton->setIcon(icon); }
void FunctionKeyBar::setLeadingToolTip(const QString &text) { m_leadingButton->setToolTip(text); }
void FunctionKeyBar::setTrailingToolTip(const QString &text) { m_trailingButton->setToolTip(text); }

QRect FunctionKeyBar::leadingButtonGlobalRect() const {
    return QRect(m_leadingButton->mapToGlobal(QPoint(0, 0)), m_leadingButton->size());
}

QRect FunctionKeyBar::trailingButtonGlobalRect() const {
    return QRect(m_trailingButton->mapToGlobal(QPoint(0, 0)), m_trailingButton->size());
}
