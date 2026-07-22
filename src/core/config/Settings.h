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

    // Single source of truth for where configuration lives, so every store
    // (Settings, ConnectionStore, SessionManager) agrees on the directory
    // instead of each duplicating the path logic.
    static QString configDir();       // ~/.config/totalcommander (created if missing)
    static QString configFilePath();  // <configDir>/config.ini

    Theme theme() const;
    void setTheme(Theme theme);

    QString language() const;
    void setLanguage(const QString &language);

    // Point size of the file-list font, shared by both panels. Clamped to
    // 8..18; defaults to 12.
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

    // When true, entering an archive (zip/7z/tar/...) browses it in place as if
    // it were a folder. When false, archives are treated as plain files (opened
    // with the default handler / previewed). Defaults to true.
    bool archiveAsFolder() const;
    void setArchiveAsFolder(bool on);

    QByteArray windowGeometry() const;
    void setWindowGeometry(const QByteArray &geometry);

    // File-list header layout (column widths + sort indicator), shared by both
    // panels, from QHeaderView::saveState().
    QByteArray viewHeaderState() const;
    void setViewHeaderState(const QByteArray &state);

    // Visibility of the optional UI bars/panes (View menu toggles). The command
    // line and function-key bar default on; the folder tree defaults off.
    bool showCommandBar() const;
    void setShowCommandBar(bool show);
    bool showFunctionKeyBar() const;
    void setShowFunctionKeyBar(bool show);
    bool showFolderTree() const;
    void setShowFolderTree(bool show);

    // QSplitter::saveState() blobs for the panel divider and the folder-tree /
    // panels divider, so the layout survives a restart.
    QByteArray panelSplitterState() const;
    void setPanelSplitterState(const QByteArray &state);
    QByteArray outerSplitterState() const;
    void setOuterSplitterState(const QByteArray &state);

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

    // Number of provider (SFTP/FTP/WebDAV) transfers OperationQueue may run
    // concurrently. Clamped to 1..8; defaults to 2. Local filesystem
    // operations are unaffected by this setting and always run one at a time.
    int maxConcurrentTransfers() const;
    void setMaxConcurrentTransfers(int count);

private:
    QSettings m_settings;
};
