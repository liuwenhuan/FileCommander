#pragma once

#include <QObject>
#include <QSharedPointer>
#include <QString>
#include <QStringList>
#include <QVector>

// Per-tab state. Deliberately stored via QSharedPointer<TabState> in a
// QVector (never by value in a QList) -- the old project's TabManager
// crashed on destruction from exactly that pattern (double-free of the
// QStringList members when a QList<TabState> tore itself down).
struct TabState {
    QString path;
    QStringList selectedFiles;
    int sortColumn = 0;
    int sortOrder = 0; // Qt::AscendingOrder / Qt::DescendingOrder
};

// Owns the tab state for one FilePanel. Purely data/state -- the visual
// QTabBar (see TabBar.h) is a separate widget that FilePanel keeps in
// sync with this.
class TabManager : public QObject {
    Q_OBJECT

public:
    explicit TabManager(QObject *parent = nullptr);

    int addTab(const QString &path);
    void closeTab(int index);
    void closeOthers(int index);
    void closeToRight(int index);

    void setActiveIndex(int index);
    int activeIndex() const { return m_activeIndex; }
    int count() const { return m_tabs.size(); }

    QSharedPointer<TabState> tabAt(int index) const;
    QSharedPointer<TabState> activeTab() const;

signals:
    void tabAdded(int index);
    void tabClosed(int index);
    void activeChanged(int index);

private:
    QVector<QSharedPointer<TabState>> m_tabs;
    int m_activeIndex = -1;
};
