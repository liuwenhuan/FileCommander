#include "BreadcrumbBar.h"

#include <QDir>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QShortcut>
#include <QStackedLayout>

BreadcrumbBar::BreadcrumbBar(QWidget *parent) : QWidget(parent) {
    // A plain QWidget subclass does not paint a stylesheet background unless
    // it is told to; without this the CRT theme's scanline texture stops at
    // the Qt-provided widgets and this one stays flat. light.qss/dark.qss
    // declare `background: transparent` for this class so their appearance is
    // unchanged -- it shows the parent, exactly as it did before.
    setAttribute(Qt::WA_StyledBackground, true);
    m_segmentsWidget = new QWidget(this);
    // Named so a theme can make it transparent: a bare QWidget IS painted with
    // the sheet's `QWidget { background }`, and this one fills the bar, so it
    // would hide any texture the bar itself was given.
    m_segmentsWidget->setObjectName(QStringLiteral("BreadcrumbSegments"));
    m_segmentsWidget->installEventFilter(this);
    m_segmentsLayout = new QHBoxLayout(m_segmentsWidget);
    m_segmentsLayout->setContentsMargins(6, 0, 6, 0);
    m_segmentsLayout->setSpacing(0);

    m_pathLabel = new QLabel(m_segmentsWidget);
    m_pathLabel->installEventFilter(this); // double-click the text to edit the path
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
    // A single click navigates via the breadcrumb segments (label links); only
    // a double-click switches to the editable path field.
    if ((watched == m_segmentsWidget || watched == m_pathLabel) &&
        event->type() == QEvent::MouseButtonDblClick) {
        enterEditMode();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void BreadcrumbBar::changeEvent(QEvent *event) {
    QWidget::changeEvent(event);
    // Theme switches arrive as a palette/style re-polish. rebuildSegments() re-
    // samples the (now updated) WindowText colour so the path text tracks the
    // active theme instead of keeping the colour frozen at the last navigation.
    if (event->type() == QEvent::PaletteChange ||
        event->type() == QEvent::StyleChange) {
        rebuildSegments();
    }
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

    const QString cleanPath = QDir::cleanPath(QDir::fromNativeSeparators(m_path));
    const bool hasDriveRoot = cleanPath.size() >= 2 && cleanPath.at(0).isLetter() &&
                              cleanPath.at(1) == QLatin1Char(':');
    QString html;
    QString accumulated;
    QString remainder = cleanPath;
    if (hasDriveRoot) {
        accumulated = cleanPath.left(2);
        html = segment(accumulated + QLatin1Char('/'), accumulated);
        remainder.remove(0, 2);
    } else {
        html = segment(QStringLiteral("/"), QStringLiteral("/"));
    }

    const QStringList parts = remainder.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (int i = 0; i < parts.size(); ++i) {
        accumulated += QLatin1Char('/') + parts.at(i);
        if (hasDriveRoot || i > 0) // "/" between segments; POSIX root already is "/"
            html += QLatin1Char('/');
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
