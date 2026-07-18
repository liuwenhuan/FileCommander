#pragma once

#include <QByteArray>
#include <QSettings>
#include <QString>

// Wraps QSettings at ~/.config/totalcommander/config.ini. Theme/language
// are stored now and acted on starting in Phase 4 (theming/i18n).
class Settings {
public:
    enum class Theme { Auto, Light, Dark };

    Settings();

    Theme theme() const;
    void setTheme(Theme theme);

    QString language() const;
    void setLanguage(const QString &language);

    bool showHiddenFiles() const;
    void setShowHiddenFiles(bool show);

    bool confirmDelete() const;
    void setConfirmDelete(bool confirm);

    bool confirmOverwrite() const;
    void setConfirmOverwrite(bool confirm);

    QByteArray windowGeometry() const;
    void setWindowGeometry(const QByteArray &geometry);

private:
    QSettings m_settings;
};
