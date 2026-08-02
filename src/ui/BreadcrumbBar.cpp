#include "BreadcrumbBar.h"

#include <QDir>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QResizeEvent>
#include <QShortcut>
#include <QShowEvent>
#include <QStackedLayout>
#include <QStyle>
#include <QTimer>
#include <QToolButton>

namespace {

class AddressRowBorderOverlay final : public QWidget {
public:
    explicit AddressRowBorderOverlay(QWidget *parent) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setAutoFillBackground(false);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        if (width() <= 0 || height() <= 0)
            return;

        QPainter painter(this);
        painter.setPen(QPen(palette().color(QPalette::Mid), 1));
        painter.drawLine(0, 0, 0, height() - 1);
        if (width() > 1)
            painter.drawLine(width() - 1, 0, width() - 1, height() - 1);
    }
};

} // namespace

BreadcrumbBar::BreadcrumbBar(QWidget *parent) : QWidget(parent) {
    // A plain QWidget subclass does not paint a stylesheet background unless
    // it is told to; without this the CRT theme's scanline texture stops at
    // the Qt-provided widgets and this one stays flat. light.qss/dark.qss
    // declare `background: transparent` for this class so their appearance is
    // unchanged -- it shows the parent, exactly as it did before.
    setAttribute(Qt::WA_StyledBackground, true);
    setProperty("editing", false);

    m_displayWidget = new QWidget(this);
    m_displayWidget->setObjectName(QStringLiteral("BreadcrumbDisplay"));
    m_displayWidget->setMinimumSize(0, 0);
    m_displayWidget->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_displayWidget->installEventFilter(this);

    m_scrollLeftButton = new QToolButton(m_displayWidget);
    m_scrollLeftButton->setObjectName(QStringLiteral("BreadcrumbScrollLeft"));
    m_scrollLeftButton->setArrowType(Qt::LeftArrow);
    m_scrollLeftButton->setAutoRaise(true);
    m_scrollLeftButton->setAutoRepeat(true);
    m_scrollLeftButton->setAutoRepeatDelay(250);
    m_scrollLeftButton->setAutoRepeatInterval(60);
    m_scrollLeftButton->setFocusPolicy(Qt::NoFocus);
    m_scrollLeftButton->setMinimumHeight(0);
    m_scrollLeftButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Ignored);
    m_scrollLeftButton->hide();

    m_scrollRightButton = new QToolButton(m_displayWidget);
    m_scrollRightButton->setObjectName(QStringLiteral("BreadcrumbScrollRight"));
    m_scrollRightButton->setArrowType(Qt::RightArrow);
    m_scrollRightButton->setAutoRaise(true);
    m_scrollRightButton->setAutoRepeat(true);
    m_scrollRightButton->setAutoRepeatDelay(250);
    m_scrollRightButton->setAutoRepeatInterval(60);
    m_scrollRightButton->setFocusPolicy(Qt::NoFocus);
    m_scrollRightButton->setMinimumHeight(0);
    m_scrollRightButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Ignored);
    m_scrollRightButton->hide();

    m_viewportWidget = new QWidget(m_displayWidget);
    m_viewportWidget->setObjectName(QStringLiteral("BreadcrumbViewport"));
    m_viewportWidget->setMinimumSize(0, 0);
    m_viewportWidget->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_viewportWidget->setFocusPolicy(Qt::NoFocus);
    m_viewportWidget->installEventFilter(this);

    m_segmentsWidget = new QWidget(m_viewportWidget);
    m_segmentsWidget->show();
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
    m_pathLabel->show();
    auto *displayLayout = new QHBoxLayout(m_displayWidget);
    displayLayout->setContentsMargins(0, 0, 0, 0);
    displayLayout->setSpacing(0);
    displayLayout->addWidget(m_scrollLeftButton);
    displayLayout->addWidget(m_viewportWidget, 1);
    displayLayout->addWidget(m_scrollRightButton);

    connect(m_scrollLeftButton, &QToolButton::clicked, this, [this] {
        scrollBy(-qMax(40, m_viewportWidget->width() * 2 / 3));
    });
    connect(m_scrollRightButton, &QToolButton::clicked, this, [this] {
        scrollBy(qMax(40, m_viewportWidget->width() * 2 / 3));
    });

    m_editLine = new QLineEdit(this);
    connect(m_editLine, &QLineEdit::returnPressed, this, [this]() { exitEditMode(true); });
    auto *escapeShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), m_editLine);
    escapeShortcut->setContext(Qt::WidgetShortcut);
    connect(escapeShortcut, &QShortcut::activated, this, [this]() { exitEditMode(false); });

    m_stack = new QStackedLayout(this);
    setContentsMargins(0, 0, 0, 0);
    m_stack->setContentsMargins(0, 0, 0, 0);
    m_stack->addWidget(m_displayWidget); // index 0: breadcrumb display
    m_stack->addWidget(m_editLine);       // index 1: manual path entry
    m_stack->setCurrentIndex(0);

    // The enclosing PanelAddressRow owns the only address-toolbar borders.
    // This widget is the path control, so it must not add an inner frame.
    setStyleSheet(QStringLiteral("BreadcrumbBar { border: none; }"));
}

bool BreadcrumbBar::eventFilter(QObject *watched, QEvent *event) {
    // A single click navigates via the breadcrumb segments (label links); only
    // a double-click switches to the editable path field.
    if ((watched == m_viewportWidget || watched == m_segmentsWidget ||
         watched == m_pathLabel) &&
        event->type() == QEvent::MouseButtonDblClick) {
        enterEditMode();
        return true;
    }
    if ((watched == m_displayWidget || watched == m_viewportWidget) &&
        event->type() == QEvent::Resize) {
        updateOverflowControls();
    }
    if (watched == m_addressRow &&
        (event->type() == QEvent::Resize || event->type() == QEvent::PaletteChange ||
         event->type() == QEvent::StyleChange)) {
        updateAddressRowBorder();
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
        QTimer::singleShot(0, this, [this] { updateOverflowControls(); });
    }
}

void BreadcrumbBar::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    updateOverflowControls();
}

void BreadcrumbBar::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);

    QWidget *addressRow = parentWidget();
    if (!addressRow || addressRow->objectName() != QStringLiteral("PanelAddressRow"))
        return;

    if (m_addressRow != addressRow) {
        if (m_addressRow)
            m_addressRow->removeEventFilter(this);
        m_addressRow = addressRow;
        m_addressRow->installEventFilter(this);
    }
    if (!m_addressRowBorder)
        m_addressRowBorder = new AddressRowBorderOverlay(m_addressRow);
    updateAddressRowBorder();
}

void BreadcrumbBar::setPath(const QString &path) {
    m_path = path;
    rebuildSegments();
    if (m_stack->currentIndex() != 0) {
        m_stack->setCurrentIndex(0);
        updateEditingProperty(false);
    }
    m_scrollOffset = 0;
    QTimer::singleShot(0, this, [this] { updateOverflowControls(); });
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

    const QSize contentSize(m_pathLabel->sizeHint().width() + 12,
                            qMax(m_pathLabel->sizeHint().height(),
                                 m_segmentsLayout->sizeHint().height()));
    m_segmentsWidget->resize(contentSize.width(),
                             qMax(contentSize.height(), m_viewportWidget->height()));
}

void BreadcrumbBar::enterEditMode() {
    m_editLine->setText(m_path);
    m_stack->setCurrentIndex(1);
    updateEditingProperty(true);
    m_editLine->setFocus();
    m_editLine->selectAll();
}

void BreadcrumbBar::exitEditMode(bool commit) {
    if (commit)
        emit pathActivated(m_editLine->text());
    else {
        m_stack->setCurrentIndex(0);
        updateEditingProperty(false);
        updateOverflowControls();
    }
}

void BreadcrumbBar::scrollBy(int distance) {
    const int contentWidth = m_pathLabel->sizeHint().width() + 12;
    const int maximum = qMax(0, contentWidth - m_viewportWidget->width());
    m_scrollOffset = qBound(0, m_scrollOffset + distance, maximum);
    updateOverflowControls();
}

void BreadcrumbBar::updateOverflowControls() {
    const bool displayMode = m_stack->currentIndex() == 0;
    const int contentWidth = m_pathLabel->sizeHint().width() + 12;
    const bool overflow = displayMode && contentWidth > width();
    m_scrollLeftButton->setVisible(overflow);
    m_scrollRightButton->setVisible(overflow);
    if (m_displayWidget->layout())
        m_displayWidget->layout()->activate();
    const int maximum = qMax(0, contentWidth - m_viewportWidget->width());
    m_scrollOffset = qBound(0, m_scrollOffset, maximum);
    if (!overflow)
        m_scrollOffset = 0;
    m_segmentsWidget->setGeometry(-m_scrollOffset, 0, contentWidth,
                                  m_viewportWidget->height());
    m_scrollLeftButton->setEnabled(overflow && m_scrollOffset > 0);
    m_scrollRightButton->setEnabled(overflow && m_scrollOffset < maximum);
}

void BreadcrumbBar::updateEditingProperty(bool editing) {
    setProperty("editing", editing);
    style()->unpolish(this);
    style()->polish(this);
    update();
}

void BreadcrumbBar::updateAddressRowBorder() {
    if (!m_addressRow || !m_addressRowBorder)
        return;

    m_addressRowBorder->setGeometry(m_addressRow->rect());
    m_addressRowBorder->show();
    m_addressRowBorder->raise();
    m_addressRowBorder->update();
}
