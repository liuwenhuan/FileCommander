#pragma once

#include <QObject>
#include <QPalette>

#include "Settings.h"

// Applies the light/dark/auto QSS theme application-wide. "Auto" follows
// the desktop's own light/dark preference, inferred from the default
// QPalette captured before any stylesheet is applied (Qt5 has no
// QStyleHints::colorScheme() -- that's Qt6 -- so this is a lightness
// heuristic on the system window color instead).
class ThemeManager : public QObject {
    Q_OBJECT

public:
    explicit ThemeManager(QObject *parent = nullptr);

    void apply(Settings::Theme theme);
    Settings::Theme requestedTheme() const { return m_requestedTheme; }

private:
    bool systemPrefersDark() const;

    Settings::Theme m_requestedTheme = Settings::Theme::Auto;
    QPalette m_originalPalette;
};
