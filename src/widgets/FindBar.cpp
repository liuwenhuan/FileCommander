#include "FindBar.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QStyle>
#include <QToolButton>

namespace {

QToolButton *makeButton(QWidget *parent, const QString &objectName, const QString &text,
                        const QString &tip, bool checkable) {
    auto *button = new QToolButton(parent);
    button->setObjectName(objectName);
    button->setText(text);
    button->setToolTip(tip);
    button->setCheckable(checkable);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus); // Enter must stay with the input
    return button;
}

QLabel *makeLabel(QWidget *parent, const QString &objectName) {
    auto *label = new QLabel(parent);
    label->setObjectName(objectName);
    label->setProperty("semanticState", QStringLiteral("muted"));
    label->hide();
    return label;
}

} // namespace

FindBar::FindBar(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("FindBar"));

    m_input = new QLineEdit(this);
    m_input->setObjectName(QStringLiteral("FindBarInput"));
    m_input->setPlaceholderText(tr("Find…"));
    m_input->setClearButtonEnabled(true);
    m_input->installEventFilter(this);

    m_hexToggle = makeButton(this, QStringLiteral("FindBarHexToggle"), tr("Hex"),
                             tr("Search a byte sequence instead of text, e.g. 4D 5A"), true);
    m_caseToggle = makeButton(this, QStringLiteral("FindBarCaseToggle"), tr("Aa"),
                              tr("Ignore case (ASCII letters only)"), true);
    m_prevButton = makeButton(this, QStringLiteral("FindBarPrevButton"), tr("Previous"),
                              tr("Previous match (Shift+Enter)"), false);
    m_nextButton = makeButton(this, QStringLiteral("FindBarNextButton"), tr("Next"),
                              tr("Next match (Enter)"), false);
    m_closeButton = makeButton(this, QStringLiteral("FindBarCloseButton"), tr("Close"),
                               tr("Close the find bar (Esc)"), false);

    m_status = makeLabel(this, QStringLiteral("FindBarStatus"));
    m_note = makeLabel(this, QStringLiteral("FindBarNote"));

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 2, 4, 2);
    layout->setSpacing(4);
    layout->addWidget(m_input, 1);
    layout->addWidget(m_hexToggle);
    layout->addWidget(m_caseToggle);
    layout->addWidget(m_prevButton);
    layout->addWidget(m_nextButton);
    layout->addWidget(m_status);
    layout->addWidget(m_note, 1);
    layout->addWidget(m_closeButton);

    connect(m_input, &QLineEdit::textChanged, this, [this] { recompile(); });
    connect(m_input, &QLineEdit::returnPressed, this,
            [this] { requestSearch(ByteSearch::Direction::Forward); });
    connect(m_nextButton, &QToolButton::clicked, this,
            [this] { requestSearch(ByteSearch::Direction::Forward); });
    connect(m_prevButton, &QToolButton::clicked, this,
            [this] { requestSearch(ByteSearch::Direction::Backward); });
    connect(m_closeButton, &QToolButton::clicked, this, [this] {
        hide();
        emit closed();
    });
    connect(m_hexToggle, &QToolButton::toggled, this, [this](bool on) {
        // Case folding is a property of characters; a hex needle has none.
        m_caseToggle->setEnabled(!on);
        recompile();
    });
    connect(m_caseToggle, &QToolButton::toggled, this, [this](bool) { recompile(); });

    recompile();
}

void FindBar::setEncoding(const QByteArray &codecName) {
    if (m_codecName == codecName)
        return;
    m_codecName = codecName;
    recompile();
}

ByteSearch::Mode FindBar::mode() const {
    return m_hexToggle->isChecked() ? ByteSearch::Mode::Hex : ByteSearch::Mode::Text;
}

void FindBar::setMode(ByteSearch::Mode mode) {
    m_hexToggle->setChecked(mode == ByteSearch::Mode::Hex);
}

bool FindBar::isCaseInsensitive() const {
    return m_caseToggle->isChecked();
}

void FindBar::setCaseInsensitive(bool on) {
    m_caseToggle->setChecked(on);
}

QString FindBar::query() const {
    return m_input->text();
}

void FindBar::setQuery(const QString &text) {
    m_input->setText(text);
}

void FindBar::showMatch(int ordinal, int total) {
    m_resultIsFailure = false;
    // Pure punctuation, so it stays out of the catalogs; "Match %1" is a real
    // sentence and does not.
    m_resultText = total >= 0 ? QStringLiteral("%1 / %2").arg(ordinal).arg(total)
                              : tr("Match %1").arg(ordinal);
    updateStatus();
}

void FindBar::showNoMatch() {
    m_resultIsFailure = true;
    m_resultText = tr("No matches");
    updateStatus();
}

void FindBar::clearResult() {
    m_resultIsFailure = false;
    m_resultText.clear();
    updateStatus();
}

void FindBar::activate() {
    show();
    raise();
    m_input->setFocus(Qt::ShortcutFocusReason);
    m_input->selectAll();
}

void FindBar::repeatSearch(ByteSearch::Direction direction) {
    requestSearch(direction);
}

void FindBar::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        event->accept();
        hide();
        emit closed();
        return;
    }
    QWidget::keyPressEvent(event);
}

bool FindBar::eventFilter(QObject *watched, QEvent *event) {
    // QLineEdit::returnPressed cannot tell Enter from Shift+Enter, and Esc
    // inside a line edit is swallowed by the clear button on some styles, so
    // both are read here before the editor sees them.
    if (watched == m_input && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Escape) {
            hide();
            emit closed();
            return true;
        }
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) &&
            (key->modifiers() & Qt::ShiftModifier)) {
            requestSearch(ByteSearch::Direction::Backward);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void FindBar::recompile() {
    const ByteSearch::CaseFolding folding = m_caseToggle->isChecked()
                                                ? ByteSearch::CaseFolding::AsciiInsensitive
                                                : ByteSearch::CaseFolding::Exact;
    m_needle = ByteSearch::compile(m_input->text(), mode(), m_codecName, folding);
    // A stale "3 / 12" beside a needle that no longer matches it is worse than
    // no readout at all.
    m_resultText.clear();
    m_resultIsFailure = false;
    updateStatus();
    emit queryChanged(m_needle);
}

void FindBar::requestSearch(ByteSearch::Direction direction) {
    if (!m_needle.valid || m_needle.isEmpty())
        return;
    emit searchRequested(m_needle, direction);
}

void FindBar::updateStatus() {
    const bool searchable = m_needle.valid && !m_needle.isEmpty();
    m_prevButton->setEnabled(searchable);
    m_nextButton->setEnabled(searchable);

    setLabel(m_status, m_resultText, m_resultIsFailure ? "error" : "muted");

    if (!m_needle.error.isEmpty())
        setLabel(m_note, m_needle.error, "error");
    else
        setLabel(m_note, m_needle.note, "warning");
}

void FindBar::setLabel(QLabel *label, const QString &text, const char *semanticState) {
    const QString state = QString::fromLatin1(semanticState);
    if (label->property("semanticState").toString() != state) {
        label->setProperty("semanticState", state);
        label->style()->unpolish(label);
        label->style()->polish(label);
    }
    label->setText(text);
    label->setToolTip(text);
    label->setVisible(!text.isEmpty());
}
