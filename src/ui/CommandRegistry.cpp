#include "CommandRegistry.h"

#include <QShortcut>
#include <QWidget>

CommandRegistry::CommandRegistry(QWidget *shortcutParent) : m_shortcutParent(shortcutParent) {}

void CommandRegistry::bind(const QString &id, const QString &label,
                           const QKeySequence &defaultSequence,
                           const QKeySequence &currentSequence, std::function<void()> handler) {
    // The label is refreshed on every call so a language-change re-run picks up
    // the new translation. The QShortcut is created once and only once: a
    // second one on the same key fires the command twice.
    m_labels[id] = label;
    if (m_shortcuts.contains(id)) {
        for (auto &entry : m_keyedOrder) {
            if (entry.first == id) {
                entry.second = label;
                break;
            }
        }
        return;
    }

    m_defaults[id] = defaultSequence;
    m_keyedOrder.append({id, label});
    m_handlers[id] = std::move(handler);

    auto *shortcut = new QShortcut(currentSequence, m_shortcutParent);
    shortcut->setContext(Qt::WindowShortcut);
    QObject::connect(shortcut, &QShortcut::activated, m_shortcutParent, m_handlers[id]);
    m_shortcuts[id] = shortcut;
}

void CommandRegistry::registerCommand(const QString &id, const QString &label,
                                      std::function<void()> handler) {
    m_labels[id] = label; // refreshed on a language-change re-run
    if (!m_handlers.contains(id))
        m_handlers[id] = std::move(handler);
}

void CommandRegistry::setDefaultSequence(const QString &id, const QKeySequence &sequence) {
    m_defaults[id] = sequence;
}

bool CommandRegistry::run(const QString &id) const {
    const auto it = m_handlers.constFind(id);
    if (it == m_handlers.constEnd() || !it.value())
        return false;
    it.value()();
    return true;
}

std::function<void()> CommandRegistry::handler(const QString &id) const {
    return m_handlers.value(id);
}

QString CommandRegistry::label(const QString &id) const {
    return m_labels.value(id, id);
}

bool CommandRegistry::contains(const QString &id) const {
    return m_labels.contains(id) || m_handlers.contains(id);
}

QKeySequence CommandRegistry::sequence(const QString &id) const {
    return m_defaults.value(id);
}

QStringList CommandRegistry::ids() const {
    return m_labels.keys();
}

QList<QPair<QString, QString>> CommandRegistry::keyedOrder() const {
    return m_keyedOrder;
}

QMap<QString, QKeySequence> CommandRegistry::defaults() const {
    return m_defaults;
}

QMap<QString, QKeySequence> CommandRegistry::currentSequences() const {
    QMap<QString, QKeySequence> current;
    for (auto it = m_shortcuts.constBegin(); it != m_shortcuts.constEnd(); ++it)
        current[it.key()] = it.value()->key();
    return current;
}

void CommandRegistry::setSequence(const QString &id, const QKeySequence &sequence) {
    if (QShortcut *shortcut = m_shortcuts.value(id))
        shortcut->setKey(sequence);
}
