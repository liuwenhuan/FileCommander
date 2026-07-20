#include "BreadcrumbBar.h"

#include <QDir>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QShortcut>
#include <QStackedLayout>

BreadcrumbBar::BreadcrumbBar(QWidget *parent) : QWidget(parent) {
    m_segmentsWidget = new QWidget(this);
    m_segmentsWidget->installEventFilter(this);
    m_segmentsLayout = new QHBoxLayout(m_segmentsWidget);
    m_segmentsLayout->setContentsMargins(6, 0, 6, 0);
    m_segmentsLayout->setSpacing(0);

    m_pathLabel = new QLabel(m_segmentsWidget);
    m_pathLabel->setTextFormat(Qt::RichText);
    m_pathLabel->setTextInteractionFlags(Qt::LinksAccessibleByMouse |
                                          Qt::LinksAccessibleByKeyboard);
    connect(m_pathLabel, &QLabel::linkActivated, this,
            [this](const QString &href) { emit pathActivated(href); });
    m_segmentsLayout->addWidget(m_pathLabel);
    m_segmentsLayout->addStretch(1);

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
    // Render the path as a normal-looking string with each segment a link,
    // styled in the regular text colour (no underline) so it reads like a
    // plain path; the pointing-hand cursor signals the segments are clickable.
    const QString colour = palette().color(QPalette::WindowText).name();
    auto segment = [&colour](const QString &target, const QString &label) {
        return QStringLiteral("<a href=\"%1\" style=\"color:%2;text-decoration:none\">%3</a>")
            .arg(target.toHtmlEscaped(), colour, label.toHtmlEscaped());
    };

    QString html = segment(QStringLiteral("/"), QStringLiteral("/"));
    const QString cleanPath = QDir::cleanPath(m_path);
    const QStringList parts = cleanPath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString accumulated;
    for (int i = 0; i < parts.size(); ++i) {
        accumulated += QLatin1Char('/') + parts.at(i);
        if (i > 0) // backslash between segments; root already ends in "/"
            html += QLatin1Char('\\');
        html += segment(accumulated, parts.at(i));
    }
    m_pathLabel->setText(html);
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
