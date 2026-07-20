#include "FunctionKeyBar.h"

#include <QHBoxLayout>
#include <QMenu>
#include <QPushButton>

FunctionKeyBar::FunctionKeyBar(QWidget *parent) : QWidget(parent) {
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 2, 4, 2);

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
}

void FunctionKeyBar::setLabel(int index, const QString &text) {
    if (index >= 0 && index < Count)
        m_buttons[index]->setText(text);
}
