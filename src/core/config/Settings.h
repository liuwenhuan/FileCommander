#pragma once

#include <QByteArray>
#include <QKeySequence>
#include <QMap>
#include <QSettings>
#include <QString>
#include <QStringList>

// Wraps QSettings at ~/.config/FileCommander/config.ini. Theme/language
// are stored now and acted on starting in Phase 4 (theming/i18n).
class Settings {
public:
    enum class Theme { Auto, Light, Dark };

    Settings();

    // Single source of truth for where configuration lives, so every store
    // (Settings, ConnectionStore, SessionManager) agrees on the directory
    // instead of each duplicating the path logic.
    static QString configDir();       // ~/.config/FileCommander (created if missing)
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

    // Audio preview volume/mute, kept independent of the video preview so the
    // audio player (which is opened to be heard) defaults to un-muted.
    int audioVolume() const;
    void setAudioVolume(int volume);
    bool audioMuted() const;
    void setAudioMuted(bool muted);

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

    // Auto-open a newly inserted removable device (USB stick, phone storage,
    // external HDD) in a fresh, activated tab. Defaults to false.
    bool autoOpenNewDevice() const;
    void setAutoOpenNewDevice(bool on);

    QByteArray windowGeometry() const;
    void setWindowGeometry(const QByteArray &geometry);

    // Legacy: file-list header layout (column widths + sort) as one
    // QHeaderView::saveState() blob shared by both panels. Superseded by the
    // per-side column settings below; kept read-only for one-time migration.
    QByteArray viewHeaderState() const;
    void setViewHeaderState(const QByteArray &state);

    // Per-side ("left"/"right") file-list column layout, persisted independently
    // for each panel. columnBaseWidths is a comma-joined list of the per-column
    // base (target) widths; hiddenColumnsMask is a bitmask of hidden columns
    // (1<<column); sortColumn/sortOrder record the active sort. Empty / -1 mean
    // "not customized yet" so the panel falls back to content-fit defaults.
    QString columnBaseWidths(const QString &side) const;
    void setColumnBaseWidths(const QString &side, const QString &csv);
    int hiddenColumnsMask(const QString &side) const;
    void setHiddenColumnsMask(const QString &side, int mask);
    int sortColumn(const QString &side) const;
    void setSortColumn(const QString &side, int column);
    int sortOrder(const QString &side) const;
    void setSortOrder(const QString &side, int order);

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

    // The two extra square toolbar buttons flanking the F-key row (slot =
    // "leading" before F3 / "trailing" after F8). Same reassignable-command
    // model as the F-keys, keyed by name instead of index.
    QString extraKeyCommand(const QString &slot, const QString &defaultId) const;
    void setExtraKeyCommand(const QString &slot, const QString &id);

    // Directory hotlist (Ctrl+D).
    QStringList favoriteDirectories() const;
    void addFavoriteDirectory(const QString &path);
    void removeFavoriteDirectory(const QString &path);

    // Number of provider (SFTP/FTP/WebDAV) transfers OperationQueue may run
    // concurrently. Clamped to 1..8; defaults to 2. Local filesystem
    // operations are unaffected by this setting and always run one at a time.
    int maxConcurrentTransfers() const;
    void setMaxConcurrentTransfers(int count);

    // Per-side ("left"/"right") thumbnail icon size and list row height, set via
    // each panel's status-bar -/+ buttons. 0 means "not customized yet" -- the
    // panel then derives the size from the View-menu font instead.
    int thumbnailIconSize(const QString &side) const;
    void setThumbnailIconSize(const QString &side, int px);
    int listRowHeight(const QString &side) const;
    void setListRowHeight(const QString &side, int height);

    // Online-update bookkeeping: the yyyy-MM-dd date of the last update check,
    // so the background check runs only once on the first launch of a given day.
    QString updateLastCheckDate() const;
    void setUpdateLastCheckDate(const QString &date);

    // Whether the quick-notepad third column was open when the app last closed,
    // so it reappears on restart.
    bool notepadVisible() const;
    void setNotepadVisible(bool on);

    // Persisted height (px) of the quick-notepad editor pane, i.e. the split
    // the user dragged between the note list (top) and the editor (bottom).
    // The list absorbs the remaining space, so saving the editor height keeps
    // the divider stable across the popup's dynamic total-height changes.
    int notepadEditorHeight() const;
    void setNotepadEditorHeight(int height);

private:
    QSettings m_settings;
};
