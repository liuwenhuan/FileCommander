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
    // Persisted as the raw int, so new themes must be APPENDED -- reordering
    // would silently repaint every existing install.
    enum class Theme { Auto, Light, Dark, Crt };

    Settings();

    // Single source of truth for where configuration lives, so every store
    // (Settings, ConnectionStore, SessionManager) agrees on the directory
    // instead of each duplicating the path logic.
    static QString configDir();       // ~/.config/FileCommander (created if missing)
    static QString configFilePath();  // <configDir>/config.ini

    Theme theme() const;
    void setTheme(Theme theme);

    // Whether *content* -- thumbnails, image/PDF/slide previews, video -- is
    // recoloured to the phosphor hue along with the chrome. Only consulted for
    // Theme::Crt; the other themes never tint content whatever this says, so
    // the menu entry is disabled outside the CRT theme rather than hidden (a
    // hidden toggle reads as a missing feature). Defaults on: someone who chose
    // the CRT theme asked for a CRT.
    bool phosphorImages() const;
    void setPhosphorImages(bool on);

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

    // Cap on the thumbnail *disk* cache in MB, enforced by
    // ThumbnailCache::pruneToLimit(). Clamped to 64..8192; defaults to 512.
    //
    // The default is a backstop, not a routine evictor, and that is what the
    // size is chosen to buy. Lowering it has a cost that is not obvious: the
    // disk cache is what lets the zoom steps share stored bitmaps, so a cache
    // too small to hold a directory's rungs turns every zoom change back into a
    // full regeneration -- measured on 12 remote videos across the ten zoom
    // steps, 1804 ms served from cache against 6921 ms regenerating.
    //
    // Sized against measured per-file totals rather than per-bitmap ones (69
    // real photographs and clips, whole zoom range walked; the rung-by-rung
    // spread is in ThumbnailCache.h, and the disabled
    // MeasuresRealCacheFootprint test re-derives it):
    //
    //   * a distinct file costs 93 KB across all three rungs it can reach at
    //     the common HiDPI ratios (dpr 1.25..2.0), and 174 KB at the 90th
    //     percentile of that corpus -- detailed photographs, which are the
    //     files a cap actually has to survive;
    //   * so a real browsing history the size of the one sampled here -- 4574
    //     stored files under the old exact-size scheme, roughly 900..1500
    //     distinct files once the duplicated sizes are collapsed -- lands
    //     somewhere between 150 and 260 MB under this scheme;
    //   * 512 MB (evicting down to 410 MB) is therefore about twice the
    //     observed working set: normal use never reaches it, and a directory
    //     the user is still browsing is never evicted out from under them.
    //
    // 256 MB was considered and rejected: its 205 MB floor sits INSIDE that
    // 150..260 MB band, so ordinary use would start evicting live entries.
    int thumbnailCacheLimitMb() const;
    void setThumbnailCacheLimitMb(int mb);

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
