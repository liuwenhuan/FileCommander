#pragma once

#include <QWidget>

class QLineEdit;
class QHBoxLayout;
class QStackedLayout;

// Explorer/Nautilus-style clickable path breadcrumb: shows the current
// path as a row of per-segment buttons (click one to jump to that
// level); clicking blank space past the last segment switches to a
// plain editable QLineEdit for typing a path directly, same as before
// this widget existed. Escape while editing reverts without navigating.
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
    QLineEdit *m_editLine;
};
