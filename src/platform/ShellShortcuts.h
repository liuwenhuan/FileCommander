#pragma once

#include <QString>

#include "PlatformResult.h"

namespace fc::ShellShortcuts {

// Places a shortcut to a launchable file can be sent to.
//
// Windows and Linux each support two of these, and they are not the same two:
// Windows has a Startup folder and no menu to speak of; Linux has an
// applications menu and starts things through a different mechanism entirely.
// A destination this platform does not implement is reported Unsupported by
// create() and returns an empty locationFor(), which is what the menu asks
// before offering it.
//
// Deliberately NOT including "pin to taskbar". Windows 10 removed the ability
// for any program but Explorer to do it: the `pintaskbar` verb was withdrawn,
// and IPinnedList3 -- the interface Explorer uses -- is undocumented and
// refuses callers it cannot verify. What circulates as a workaround is
// registry forgery plus impersonating Explorer, which anti-malware treats as
// exactly what it looks like and which breaks on updates.
enum class Destination {
    Desktop,      // the user's Desktop folder (both platforms)
    Startup,      // run at sign-in (Windows only)
    Applications, // the applications menu (Linux only)
};

// Whether this platform makes shortcuts at all.
bool isSupported();

// Whether `where` is one this platform implements. The menu builds its entries
// from this, so an unimplemented destination is absent rather than failing when
// clicked.
bool supports(Destination where);

// Creates a shortcut to `targetPath` in `where`, named after the target's base
// name. An existing shortcut of that name is replaced -- the user asked for it
// to be there, and a second copy named "app (2)" would be worse than refreshing
// the one already present.
//
// `targetPath` must be a path on this machine; a caller holding a panel path has
// to establish that first (see FileProvider::isLocalFilesystem).
PlatformResult create(const QString &targetPath, Destination where);

// Where create() would put it, for telling the user afterwards. Empty when the
// destination is unsupported or the folder cannot be located.
QString locationFor(Destination where);

// Whether `path` is something this platform can be pointed at with a shortcut:
// an .exe on Windows; an ELF executable or an AppImage on Linux. Answered from
// the file's own contents where the name does not settle it, since a Linux
// binary usually has no extension at all.
bool isLaunchable(const QString &path);

// True when `path` is an AppImage that is NOT executable -- the state a browser
// download leaves it in, and the reason "nothing happens when I run it" is the
// most common first experience of the format. False for anything else,
// including on Windows, so a caller can offer to fix exactly this case.
bool needsExecutableBit(const QString &path);

// Adds the owner/group/other execute bits to `path`, leaving read and write
// alone. Used only after the user agrees to it.
PlatformResult makeExecutable(const QString &path);

} // namespace fc::ShellShortcuts
