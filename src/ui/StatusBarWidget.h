#pragma once

#include <QWidget>

class QLabel;
class QToolButton;

// Bottom status strip: selection summary for the active panel.
class StatusBarWidget : public QWidget {
    Q_OBJECT

public:
    explicit StatusBarWidget(QWidget *parent = nullptr);

    void setSelectionInfo(int selectedCount, qint64 selectedBytes, int totalCount);
    void setDiskInfo(qint64 freeBytes, qint64 totalBytes);

signals:
    // The trailing "-"/"+" buttons: shrink/grow the panel's current view (list
    // row height or thumbnail size, depending on mode). FilePanel owns the
    // mode-specific logic; this widget only reports the click.
    void zoomOutRequested();
    void zoomInRequested();

private:
    QLabel *m_label;
    QLabel *m_diskLabel;
    QToolButton *m_zoomOutButton;
    QToolButton *m_zoomInButton;
};
