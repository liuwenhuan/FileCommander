#include "FunctionKeyBar.h"

#include <QHBoxLayout>
#include <QPushButton>

namespace {
QPushButton *makeButton(QWidget *parent, const QString &text) {
    auto *btn = new QPushButton(text, parent);
    btn->setFocusPolicy(Qt::NoFocus); // keep function-key buttons out of the Tab chain
    return btn;
}
} // namespace

FunctionKeyBar::FunctionKeyBar(QWidget *parent) : QWidget(parent) {
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 2, 4, 2);

    auto *view = makeButton(this, tr("F3 View"));
    auto *edit = makeButton(this, tr("F4 Edit"));
    auto *copy = makeButton(this, tr("F5 Copy"));
    auto *move = makeButton(this, tr("F6 Move"));
    auto *mkdir = makeButton(this, tr("F7 MkDir"));
    auto *del = makeButton(this, tr("F8 Delete"));

    for (QPushButton *btn : {view, edit, copy, move, mkdir, del})
        layout->addWidget(btn);

    connect(view, &QPushButton::clicked, this, &FunctionKeyBar::viewRequested);
    connect(edit, &QPushButton::clicked, this, &FunctionKeyBar::editRequested);
    connect(copy, &QPushButton::clicked, this, &FunctionKeyBar::copyRequested);
    connect(move, &QPushButton::clicked, this, &FunctionKeyBar::moveRequested);
    connect(mkdir, &QPushButton::clicked, this, &FunctionKeyBar::mkdirRequested);
    connect(del, &QPushButton::clicked, this, &FunctionKeyBar::deleteRequested);
}
