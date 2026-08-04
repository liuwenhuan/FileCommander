#pragma once

#include <QByteArray>
#include <QWidget>

#include "ByteSearch.h"

class QLabel;
class QLineEdit;
class QToolButton;

// The find strip: an input, a text/hex switch, a case switch, next/previous, a
// match counter, Enter to repeat, Esc to close.
//
// It does not own, hold or search a document. It compiles what the user typed
// into a ByteSearch::Needle and emits it; the host owns the bytes, performs the
// search and reports the outcome back through showMatch()/showNoMatch(). That
// split is what lets the same strip sit above the F3 viewer's read-only buffer,
// the F4 editor's edit buffer and a hex pane without any of them leaking into
// the others -- and it is why the widget can be tested without a document.
//
// Wiring, minimally:
//     auto *bar = new FindBar(this);
//     bar->setEncoding(currentCodecName());          // whenever it changes
//     connect(bar, &FindBar::searchRequested, this, &Host::runSearch);
//     connect(bar, &FindBar::closed, this, &Host::hideFindBar);
//     // F3 / Ctrl+F:
//     bar->activate();                                // or bar->repeatSearch()
// and in Host::runSearch(needle, direction):
//     const int hit = ByteSearch::find(buffer, needle, cursorOffset, direction, true);
//     if (hit < 0) bar->showNoMatch();
//     else { select(hit, needle.bytes.size());
//            bar->showMatch(ByteSearch::ordinalAt(buffer, needle, hit),
//                           ByteSearch::countMatches(buffer, needle)); }
//
// Theming comes from the palette and the app stylesheet only -- no colour is
// written here. Styleable selectors: the FindBar class itself, and by object
// name FindBarInput, FindBarHexToggle, FindBarCaseToggle, FindBarPrevButton,
// FindBarNextButton, FindBarCloseButton, FindBarStatus, FindBarNote. The two
// labels also carry the app-wide `semanticState` dynamic property
// (muted/warning/error), which all three themes already style.
class FindBar : public QWidget {
    Q_OBJECT
public:
    explicit FindBar(QWidget *parent = nullptr);

    // Codec the host is reading the document as. A TEXT needle is encoded with
    // it, so getting this wrong is the difference between finding everything
    // and finding nothing. Defaults to UTF-8.
    void setEncoding(const QByteArray &codecName);
    QByteArray encoding() const { return m_codecName; }

    ByteSearch::Mode mode() const;
    void setMode(ByteSearch::Mode mode);

    bool isCaseInsensitive() const;
    void setCaseInsensitive(bool on);

    QString query() const;
    void setQuery(const QString &text);

    // The needle for what is currently typed. Invalid when the box is empty or
    // the hex is malformed; `error`/`note` are already being shown by the bar.
    ByteSearch::Needle needle() const { return m_needle; }

    // Host reports the outcome. `total` may be -1 when counting every match
    // would be too expensive to be worth it; the ordinal is then shown alone.
    void showMatch(int ordinal, int total);
    void showNoMatch();
    void clearResult();

    // Show, raise, focus and select-all -- the F3/Ctrl+F entry point.
    void activate();
    // Re-run whatever is in the box (the host's F3-again binding).
    void repeatSearch(ByteSearch::Direction direction = ByteSearch::Direction::Forward);

signals:
    // The user asked for a search. Always carries a valid, non-empty needle.
    void searchRequested(const ByteSearch::Needle &needle, ByteSearch::Direction direction);
    // The typed text, mode, case switch or encoding changed. Hosts that
    // highlight matches live use this to drop the old highlight.
    void queryChanged(const ByteSearch::Needle &needle);
    // Esc, or the close button. The host decides what "close" means.
    void closed();

protected:
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void recompile();
    void requestSearch(ByteSearch::Direction direction);
    void updateStatus();
    void setLabel(QLabel *label, const QString &text, const char *semanticState);

    QLineEdit *m_input = nullptr;
    QToolButton *m_hexToggle = nullptr;
    QToolButton *m_caseToggle = nullptr;
    QToolButton *m_prevButton = nullptr;
    QToolButton *m_nextButton = nullptr;
    QToolButton *m_closeButton = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_note = nullptr;

    QByteArray m_codecName = QByteArrayLiteral("UTF-8");
    ByteSearch::Needle m_needle;
    QString m_resultText;
    bool m_resultIsFailure = false;
};
