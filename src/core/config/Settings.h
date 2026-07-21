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

    // Point size of the file-list font, shared by both panels. Clamped to
    // 7..24 on write; defaults to 10.
    int listFontSize() const;
    void setListFontSize(int pt);

    // Video-preview playback state, persisted so later previews reuse it.
    // Speed defaults to 1.0; volume 0..100 (default 70); muted defaults to true
    // (a fresh install previews silently until the user unmutes).
    double videoSpeed() const;
    void setVideoSpeed(double speed);
    int videoVolume() const;
    void setVideoVolume(int volume);
    bool videoMuted() const;
    void setVideoMuted(bool muted);

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

    // Which command each function key (F3..F8, index 0..5) runs.
    QString functionKeyCommand(int index, const QString &defaultId) const;
    void setFunctionKeyCommand(int index, const QString &id);

    // Directory hotlist (Ctrl+D).
    QStringList favoriteDirectories() const;
    void addFavoriteDirectory(const QString &path);
    void removeFavoriteDirectory(const QString &path);

private:
    QSettings m_settings;
};
