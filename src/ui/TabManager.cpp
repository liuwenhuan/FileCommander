#include "TabManager.h"

TabManager::TabManager(QObject *parent) : QObject(parent) {}

int TabManager::addTab(const QString &path) {
    auto state = QSharedPointer<TabState>::create();
    state->path = path;
    m_tabs.append(state);
    const int index = m_tabs.size() - 1;
    emit tabAdded(index);
    return index;
}

void TabManager::closeTab(int index) {
    if (index < 0 || index >= m_tabs.size())
        return;
    m_tabs.remove(index);
    if (m_activeIndex >= m_tabs.size())
        m_activeIndex = m_tabs.size() - 1;
    emit tabClosed(index);
}

void TabManager::closeOthers(int index) {
    if (index < 0 || index >= m_tabs.size())
        return;
    QSharedPointer<TabState> keep = m_tabs.at(index);
    m_tabs.clear();
    m_tabs.append(keep);
    m_activeIndex = 0;
    emit activeChanged(0);
}

void TabManager::closeToRight(int index) {
    if (index < 0 || index >= m_tabs.size())
        return;
    while (m_tabs.size() > index + 1)
        m_tabs.remove(m_tabs.size() - 1);
    if (m_activeIndex >= m_tabs.size())
        m_activeIndex = m_tabs.size() - 1;
}

void TabManager::setActiveIndex(int index) {
    if (index < 0 || index >= m_tabs.size())
        return;
    m_activeIndex = index;
    emit activeChanged(index);
}

QSharedPointer<TabState> TabManager::tabAt(int index) const {
    if (index < 0 || index >= m_tabs.size())
        return {};
    return m_tabs.at(index);
}

QSharedPointer<TabState> TabManager::activeTab() const {
    return tabAt(m_activeIndex);
}
