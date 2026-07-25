# Bundled-runtime fixups, sourced by AppRun before the app starts.
# Installed into the AppDir as apprun-hooks/ttc-hook.sh by build-appimage.sh.

# --- Platform plugin --------------------------------------------------------
# Deepin/UOS sessions export QT_QPA_PLATFORM=dxcb (or "dxcb;xcb"), naming a
# platform plugin that only exists in the host's Qt installation. The bundled Qt
# has no dxcb, so an inherited value aborts the app outright:
#   "This application failed to start because no Qt platform plugin could be
#    initialized. Available platform plugins are: xcb."
# The ";xcb" form survives via Qt's own fallback, but the bare form is fatal.
#
# Drop any plugin name we didn't bundle. Anything left is kept, so a user who
# deliberately sets a supported platform still gets it. src/main.cpp defaults to
# xcb when the variable ends up empty.
if [ -n "${QT_QPA_PLATFORM:-}" ]; then
    _ttc_kept=""
    _ttc_plugin_dir="$this_dir/usr/plugins/platforms"
    # The value is a ';'-separated preference list.
    _ttc_ifs_saved="$IFS"
    IFS=';'
    for _ttc_p in $QT_QPA_PLATFORM; do
        [ -n "$_ttc_p" ] || continue
        if [ -f "$_ttc_plugin_dir/libq${_ttc_p}.so" ]; then
            _ttc_kept="${_ttc_kept:+$_ttc_kept;}$_ttc_p"
        fi
    done
    IFS="$_ttc_ifs_saved"

    if [ -z "$_ttc_kept" ]; then
        unset QT_QPA_PLATFORM
    else
        export QT_QPA_PLATFORM="$_ttc_kept"
    fi
    unset _ttc_kept _ttc_plugin_dir _ttc_p _ttc_ifs_saved
fi

# Same story for the platform theme: deepin/dde themes live in the host's Qt.
# A missing theme is not fatal, only noisy, but drop the known-absent ones so
# the app doesn't spend startup looking for them.
case "${QT_QPA_PLATFORMTHEME:-}" in
    deepin|dde|uos)
        unset QT_QPA_PLATFORMTHEME
        ;;
esac

# --- Bundled helper tools ---------------------------------------------------
# ttc shells out to 7z (UDF disc images) and unsquashfs (AppImage browsing).
# Put the bundled copies first so those features work on a host without them.
export PATH="$this_dir/usr/bin:$PATH"

# The AppImage runtime points LD_LIBRARY_PATH at the bundled libraries. Child
# processes inherit it, so a *host* binary launched from ttc (ffmpeg, gio, a
# terminal emulator, the user's "Open With" choice) can load our libstdc++/glib
# and fail. Stash the original so a launcher can restore it; see docs/PACKAGING.md
# for the known limitation.
export TTC_HOST_LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"
