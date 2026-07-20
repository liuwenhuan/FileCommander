#pragma once

#include <QWidget>

class QLabel;

// Bottom status strip: selection summary for the active panel.
class StatusBarWidget : public QWidget {
    Q_OBJECT

public:
    explicit StatusBarWidget(QWidget *parent = nullptr);

    void setSelectionInfo(int selectedCount, qint64 selectedBytes, int totalCount);
    void setDiskInfo(qint64 freeBytes, qint64 totalBytes);

private:
    QLabel *m_label;
    QLabel *m_diskLabel;
};
