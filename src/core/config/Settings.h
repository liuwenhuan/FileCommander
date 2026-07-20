#pragma once

#include <QByteArray>
#include <QKeySequence>
#include <QMap>
#include <QSettings>
#include <QString>
#include <QStringList>

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

    // File-list header layout (column widths + sort indicator), shared by both
    // panels, from QHeaderView::saveState().
    QByteArray viewHeaderState() const;
    void setViewHeaderState(const QByteArray &state);

    // Per-action keyboard shortcut overrides, keyed by a stable action id
    // (e.g. "copy", "newTab"). Returns defaultSeq if nothing was saved.
    QKeySequence shortcut(const QString &actionId, const QKeySequence &defaultSeq) const;
    void setShortcut(const QString &actionId, const QKeySequence &seq);
    void clearShortcutOverrides();

    // Directory hotlist (Ctrl+D).
    QStringList favoriteDirectories() const;
    void addFavoriteDirectory(const QString &path);
    void removeFavoriteDirectory(const QString &path);

private:
    QSettings m_settings;
};
