#pragma once

#include <QWidget>

class QLabel;
class QLineEdit;
class QHBoxLayout;
class QStackedLayout;

// Compact clickable path: renders the current path as a normal-looking
// backslash-separated string (e.g. /home\deepin) where each segment is a
// link you can click to jump to that level. Clicking the blank area past
// the path switches to a plain editable QLineEdit for typing a path
// directly. Escape while editing reverts without navigating.
class BreadcrumbBar : public QWidget {
    Q_OBJECT

public:
    explicit BreadcrumbBar(QWidget *parent = nullptr);

    void setPath(const QString &path);
    QString path() const { return m_path; }

signals:
    // Emitted when a segment is clicked, or the edit line is confirmed
    // with Enter.
    void pathActivated(const QString &path);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void rebuildSegments();
    void enterEditMode();
    void exitEditMode(bool commit);

    QString m_path;
    QStackedLayout *m_stack;
    QWidget *m_segmentsWidget;
    QHBoxLayout *m_segmentsLayout;
    QLabel *m_pathLabel;
    QLineEdit *m_editLine;
};
