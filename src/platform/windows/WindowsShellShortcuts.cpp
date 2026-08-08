#include "ShellShortcuts.h"

#include <QDir>
#include <QFileInfo>

#include <windows.h>
#include <objbase.h>
#include <shlobj.h>

namespace {

// Same shape as the one in WindowsTrashService.cpp. Kept local rather than
// shared: it is four lines, and hoisting it would put a COM detail into a
// header every platform includes.
class ComScope {
public:
    ComScope() : result(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComScope() {
        if (SUCCEEDED(result))
            CoUninitialize();
    }
    ComScope(const ComScope &) = delete;
    ComScope &operator=(const ComScope &) = delete;
    HRESULT result;
};

QString knownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &raw)))
        return {};
    const QString path = QString::fromWCharArray(raw);
    CoTaskMemFree(raw);
    return path;
}

} // namespace

namespace fc::ShellShortcuts {

bool isSupported() {
    return true;
}

bool supports(Destination where) {
    // No Applications: Windows' Start menu is a folder too, but putting entries
    // there is an installer's job, not a file manager's one-click action.
    return where == Destination::Desktop || where == Destination::Startup;
}

bool isLaunchable(const QString &path) {
    if (path.isEmpty())
        return false;
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile())
        return false;
    // Only .exe. A shortcut to a .bat or .cmd works, but those are far more
    // often something being edited than something wanted on the desktop, and a
    // .lnk to a .lnk is meaningless. Narrow rather than guessing.
    return info.suffix().compare(QLatin1String("exe"), Qt::CaseInsensitive) == 0;
}

// Both Windows no-ops: the execute bit is a POSIX permission, and nothing here
// is gated on it.
bool needsExecutableBit(const QString &) {
    return false;
}

PlatformResult makeExecutable(const QString &) {
    return PlatformResult::failure(PlatformError::Unsupported,
                                   QStringLiteral("Windows has no execute permission bit."));
}

QString locationFor(Destination where) {
    if (!supports(where))
        return {};
    // FOLDERID_Startup, not the registry's Run key. Both start a program at
    // sign-in, but this one is a folder the user can open, look at, and delete
    // from -- and Windows' own startup-apps UI lists it. A Run entry is
    // invisible unless you know to go looking for it, which is the wrong
    // property for something a file manager put there on one click.
    return knownFolder(where == Destination::Desktop ? FOLDERID_Desktop : FOLDERID_Startup);
}

PlatformResult create(const QString &targetPath, Destination where) {
    if (!supports(where))
        return PlatformResult::failure(PlatformError::Unsupported,
                                       QStringLiteral("Not available on this platform."));

    const QFileInfo target(targetPath);
    if (!target.exists() || !target.isFile())
        return PlatformResult::failure(PlatformError::NotFound,
                                       QStringLiteral("No such file: %1").arg(targetPath));

    const QString folder = locationFor(where);
    if (folder.isEmpty())
        return PlatformResult::failure(PlatformError::NativeFailure,
                                       QStringLiteral("Windows did not report the folder path."));

    const QString linkPath = QDir(folder).filePath(target.completeBaseName() + QLatin1String(".lnk"));

    ComScope com;
    if (FAILED(com.result) && com.result != RPC_E_CHANGED_MODE)
        return PlatformResult::failure(PlatformError::NativeFailure,
                                       QStringLiteral("COM could not be initialised."),
                                       com.result);

    IShellLinkW *link = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IShellLinkW, reinterpret_cast<void **>(&link));
    if (FAILED(hr) || !link)
        return PlatformResult::failure(PlatformError::NativeFailure,
                                       QStringLiteral("Could not create the shortcut object."), hr);

    const QString nativeTarget = QDir::toNativeSeparators(target.absoluteFilePath());
    // The working directory matters: a program started from a shortcut with none
    // inherits Explorer's, and anything it opens relative to itself then misses.
    const QString workingDir = QDir::toNativeSeparators(target.absolutePath());
    link->SetPath(reinterpret_cast<const wchar_t *>(nativeTarget.utf16()));
    link->SetWorkingDirectory(reinterpret_cast<const wchar_t *>(workingDir.utf16()));

    IPersistFile *file = nullptr;
    hr = link->QueryInterface(IID_IPersistFile, reinterpret_cast<void **>(&file));
    if (FAILED(hr) || !file) {
        link->Release();
        return PlatformResult::failure(PlatformError::NativeFailure,
                                       QStringLiteral("Could not save the shortcut."), hr);
    }

    const QString nativeLink = QDir::toNativeSeparators(linkPath);
    hr = file->Save(reinterpret_cast<const wchar_t *>(nativeLink.utf16()), TRUE);
    file->Release();
    link->Release();

    if (FAILED(hr)) {
        // Writing into Desktop or Startup is normally allowed, so a failure here
        // is usually policy or a redirected folder rather than a bad path.
        const PlatformError code =
            hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) ? PlatformError::PermissionDenied
                                                          : PlatformError::NativeFailure;
        return PlatformResult::failure(code,
                                       QStringLiteral("Could not write %1").arg(nativeLink), hr);
    }
    return PlatformResult::success();
}

} // namespace fc::ShellShortcuts
