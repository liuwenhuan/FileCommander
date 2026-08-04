#pragma once

#include <QWidget>

class QLabel;
class QLineEdit;
class QHBoxLayout;
class QResizeEvent;
class QStackedLayout;
class QToolButton;

// Compact clickable path: renders the current path as a normal-looking
// slash-separated string (e.g. /home/deepin) where each segment is a link
// you can click to jump to that level. Clicking the blank area past the
// path switches to a plain editable QLineEdit for typing a path directly.
// Escape while editing reverts without navigating.
class BreadcrumbBar : public QWidget {
    Q_OBJECT

public:
    explicit BreadcrumbBar(QWidget *parent = nullptr);

    void setPath(const QString &path);
    QString path() const { return m_path; }

    // Shows `text` as a single plain caption instead of a clickable path, for a
    // location that is not a directory anyone can type or walk up ("Computer").
    // Splitting such an identifier on "/" the way setPath does would render
    // nonsense segments that navigate nowhere. Double-clicking still opens the
    // edit line -- empty, since there is no path to pre-fill -- so the user can
    // type a real one and leave.
    void setCaption(const QString &text);

signals:
    // Emitted when a segment is clicked, or the edit line is confirmed
    // with Enter.
    void pathActivated(const QString &path);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    // The segment colours are baked into the label's rich text at build time, so
    // a theme switch (which re-polishes the palette) needs an explicit rebuild.
    void changeEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void rebuildSegments();
    void enterEditMode();
    void exitEditMode(bool commit);
    void scrollBy(int distance);
    void updateOverflowControls();
    void updateEditingProperty(bool editing);

    QString m_path;
    // Non-empty while showing a caption rather than a path; rebuildSegments()
    // renders this instead, and a theme change re-renders it in the new colour.
    QString m_caption;
    QStackedLayout *m_stack;
    QWidget *m_displayWidget;
    QWidget *m_viewportWidget;
    QWidget *m_segmentsWidget;
    QHBoxLayout *m_segmentsLayout;
    QLabel *m_pathLabel;
    QToolButton *m_scrollLeftButton;
    QToolButton *m_scrollRightButton;
    QLineEdit *m_editLine;
    int m_scrollOffset = 0;
};
