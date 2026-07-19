#include "BreadcrumbBar.h"

#include <QDir>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QShortcut>
#include <QStackedLayout>
#include <QToolButton>

BreadcrumbBar::BreadcrumbBar(QWidget *parent) : QWidget(parent) {
    m_segmentsWidget = new QWidget(this);
    m_segmentsWidget->installEventFilter(this);
    m_segmentsLayout = new QHBoxLayout(m_segmentsWidget);
    m_segmentsLayout->setContentsMargins(4, 2, 4, 2);
    m_segmentsLayout->setSpacing(2);

    m_editLine = new QLineEdit(this);
    connect(m_editLine, &QLineEdit::returnPressed, this, [this]() { exitEditMode(true); });
    auto *escapeShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), m_editLine);
    escapeShortcut->setContext(Qt::WidgetShortcut);
    connect(escapeShortcut, &QShortcut::activated, this, [this]() { exitEditMode(false); });

    m_stack = new QStackedLayout(this);
    m_stack->setContentsMargins(0, 0, 0, 0);
    m_stack->addWidget(m_segmentsWidget); // index 0: breadcrumb display
    m_stack->addWidget(m_editLine);       // index 1: manual path entry
    m_stack->setCurrentIndex(0);
}

bool BreadcrumbBar::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_segmentsWidget && event->type() == QEvent::MouseButtonPress) {
        enterEditMode();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void BreadcrumbBar::setPath(const QString &path) {
    m_path = path;
    rebuildSegments();
    if (m_stack->currentIndex() != 0)
        m_stack->setCurrentIndex(0);
}

void BreadcrumbBar::rebuildSegments() {
    QLayoutItem *item;
    while ((item = m_segmentsLayout->takeAt(0)) != nullptr) {
        delete item->widget();
        delete item;
    }

    auto addSegmentButton = [this](const QString &label, const QString &fullPath) {
        auto *btn = new QToolButton(m_segmentsWidget);
        btn->setText(label);
        btn->setAutoRaise(true);
        btn->setFocusPolicy(Qt::NoFocus);
        connect(btn, &QToolButton::clicked, this,
                [this, fullPath]() { emit pathActivated(fullPath); });
        m_segmentsLayout->addWidget(btn);
    };

    addSegmentButton(QStringLiteral("/"), QStringLiteral("/"));
    const QString cleanPath = QDir::cleanPath(m_path);
    const QStringList parts = cleanPath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString accumulated;
    for (const QString &part : parts) {
        accumulated += QLatin1Char('/') + part;
        addSegmentButton(part, accumulated);
    }
    m_segmentsLayout->addStretch(1);
}

void BreadcrumbBar::enterEditMode() {
    m_editLine->setText(m_path);
    m_stack->setCurrentIndex(1);
    m_editLine->setFocus();
    m_editLine->selectAll();
}

void BreadcrumbBar::exitEditMode(bool commit) {
    if (commit)
        emit pathActivated(m_editLine->text());
    else
        m_stack->setCurrentIndex(0);
}
