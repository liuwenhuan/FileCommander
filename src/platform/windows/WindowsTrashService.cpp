#include "TrashService.h"

#include <QByteArray>
#include <QDir>
#include <QSet>

#include <atomic>
#include <cstring>

#include <windows.h>
#include <shlobj_core.h>
#include <shobjidl.h>

namespace {
class ComScope {
public:
    ComScope() : result(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComScope() {
        if (SUCCEEDED(result))
            CoUninitialize();
    }
    HRESULT result;
};

// IFileOperation::PerformOperations() commonly returns S_OK even when an individual
// item failed (e.g. access denied) -- GetAnyOperationsAborted() only reports *that*
// something failed, not *why*, so without this sink every per-item failure was
// reported as a generic ERROR_CANCELLED and could never be classified/offered
// elevation as the permission error it actually was.
class DeleteResultSink final : public IFileOperationProgressSink {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
        if (!object)
            return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IFileOperationProgressSink) {
            *object = static_cast<IFileOperationProgressSink *>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_references; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG references = --m_references;
        if (references == 0)
            delete this;
        return references;
    }

    HRESULT STDMETHODCALLTYPE StartOperations() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT result) override { remember(result); return S_OK; }
    HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD, IShellItem *, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD, IShellItem *, LPCWSTR, HRESULT, IShellItem *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD, IShellItem *, IShellItem *, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD, IShellItem *, IShellItem *, LPCWSTR, HRESULT, IShellItem *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD, IShellItem *, IShellItem *, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD, IShellItem *, IShellItem *, LPCWSTR, HRESULT, IShellItem *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD, IShellItem *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD, IShellItem *, HRESULT result, IShellItem *) override {
        remember(result);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PreNewItem(DWORD, IShellItem *, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostNewItem(DWORD, IShellItem *, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE UpdateProgress(UINT, UINT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResetTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PauseTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResumeTimer() override { return S_OK; }

    HRESULT itemResult() const { return m_itemResult; }

private:
    void remember(HRESULT result) {
        if (FAILED(result) && SUCCEEDED(m_itemResult))
            m_itemResult = result;
    }

    std::atomic<ULONG> m_references{1};
    HRESULT m_itemResult = S_OK;
};

// IFileOperation's own failure reporting proved unreliable in practice (both
// PerformOperations()'s return value and the progress-sink callbacks above): a denied
// recycle of a UAC-protected item was still coming back mapped to ERROR_CANCELLED
// rather than ERROR_ACCESS_DENIED. This probes DELETE access directly via the plain
// Win32 API (no shell involved, no side effect since the handle is never used to
// delete anything) as a trustworthy, independent signal for the one case that matters
// for elevation eligibility.
bool deleteAccessDenied(const QString &path) {
    const QString native = QDir::toNativeSeparators(path);
    const HANDLE handle = CreateFileW(
        reinterpret_cast<LPCWSTR>(native.utf16()), DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
        return false;
    }
    return GetLastError() == ERROR_ACCESS_DENIED;
}

HRESULT invokeUndelete(IShellFolder *folder, PCUITEMID_CHILD child) {
    IContextMenu *menu = nullptr;
    PCUITEMID_CHILD children[] = {child};
    HRESULT hr = folder->GetUIObjectOf(nullptr, 1, children, IID_IContextMenu,
                                       nullptr, reinterpret_cast<void **>(&menu));
    if (FAILED(hr))
        return hr;
    HMENU commands = CreatePopupMenu();
    if (!commands)
        hr = HRESULT_FROM_WIN32(GetLastError());
    else {
        hr = menu->QueryContextMenu(commands, 0, 1, 0x7fff, CMF_NORMAL);
        if (SUCCEEDED(hr)) {
            CMINVOKECOMMANDINFO command{};
            command.cbSize = sizeof(command);
            command.lpVerb = "undelete";
            command.nShow = SW_HIDE;
            hr = menu->InvokeCommand(&command);
        }
        DestroyMenu(commands);
    }
    menu->Release();
    return hr;
}

QStringList recycleEntryTokens() {
    QStringList tokens;
    IShellFolder *desktop = nullptr;
    PIDLIST_ABSOLUTE recyclePidl = nullptr;
    IShellFolder *recycle = nullptr;
    IEnumIDList *items = nullptr;
    HRESULT hr = SHGetDesktopFolder(&desktop);
    if (SUCCEEDED(hr))
        hr = SHGetKnownFolderIDList(FOLDERID_RecycleBinFolder, KF_FLAG_DEFAULT, nullptr,
                                    &recyclePidl);
    if (SUCCEEDED(hr))
        hr = desktop->BindToObject(recyclePidl, nullptr, IID_PPV_ARGS(&recycle));
    if (SUCCEEDED(hr))
        hr = recycle->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &items);
    if (SUCCEEDED(hr)) {
        PITEMID_CHILD child = nullptr;
        ULONG fetched = 0;
        while (items->Next(1, &child, &fetched) == S_OK) {
            const UINT bytes = ILGetSize(child);
            if (bytes != 0) {
                tokens.append(QString::fromLatin1(
                    QByteArray(reinterpret_cast<const char *>(child), static_cast<int>(bytes))
                        .toBase64()));
            }
            CoTaskMemFree(child);
        }
    }
    if (items)
        items->Release();
    if (recycle)
        recycle->Release();
    CoTaskMemFree(recyclePidl);
    if (desktop)
        desktop->Release();
    return tokens;
}

HRESULT restoreEntry(const QString &entry) {
    const QByteArray bytes = QByteArray::fromBase64(entry.toLatin1());
    if (bytes.size() < int(sizeof(USHORT)))
        return E_INVALIDARG;
    auto *child = static_cast<PITEMID_CHILD>(CoTaskMemAlloc(bytes.size()));
    if (!child)
        return E_OUTOFMEMORY;
    std::memcpy(child, bytes.constData(), static_cast<size_t>(bytes.size()));
    if (ILGetSize(child) != static_cast<UINT>(bytes.size())) {
        CoTaskMemFree(child);
        return E_INVALIDARG;
    }

    IShellFolder *desktop = nullptr;
    PIDLIST_ABSOLUTE recyclePidl = nullptr;
    IShellFolder *recycle = nullptr;
    HRESULT hr = SHGetDesktopFolder(&desktop);
    if (SUCCEEDED(hr))
        hr = SHGetKnownFolderIDList(FOLDERID_RecycleBinFolder, KF_FLAG_DEFAULT, nullptr,
                                    &recyclePidl);
    if (SUCCEEDED(hr))
        hr = desktop->BindToObject(recyclePidl, nullptr, IID_PPV_ARGS(&recycle));
    if (SUCCEEDED(hr))
        hr = invokeUndelete(recycle, child);
    if (recycle)
        recycle->Release();
    CoTaskMemFree(recyclePidl);
    if (desktop)
        desktop->Release();
    CoTaskMemFree(child);
    return hr;
}

class WindowsTrashService final : public TrashService {
public:
    PlatformResult moveToTrash(const QStringList &paths) override {
        ComScope com;
        if (FAILED(com.result) && com.result != RPC_E_CHANGED_MODE)
            return failure(com.result, QStringLiteral("COM initialization failed."));
        const QStringList beforeEntries = recycleEntryTokens();
        const QSet<QString> before(beforeEntries.cbegin(), beforeEntries.cend());
        IFileOperation *operation = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&operation));
        if (FAILED(hr))
            return failure(hr, QStringLiteral("Windows recycle service is unavailable."));
        operation->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMATION |
                                     FOF_NOERRORUI | FOF_SILENT);

        auto *sink = new DeleteResultSink();
        DWORD cookie = 0;
        hr = operation->Advise(sink, &cookie);
        for (const QString &path : paths) {
            if (FAILED(hr))
                break;
            IShellItem *item = nullptr;
            const QString native = QDir::toNativeSeparators(path);
            hr = SHCreateItemFromParsingName(
                reinterpret_cast<const wchar_t *>(native.utf16()), nullptr,
                IID_PPV_ARGS(&item));
            if (SUCCEEDED(hr)) {
                hr = operation->DeleteItem(item, nullptr);
                item->Release();
            }
            if (FAILED(hr))
                break;
        }
        if (SUCCEEDED(hr))
            hr = operation->PerformOperations();
        BOOL aborted = FALSE;
        if (SUCCEEDED(hr))
            operation->GetAnyOperationsAborted(&aborted);
        if (SUCCEEDED(hr) && FAILED(sink->itemResult()))
            hr = sink->itemResult();
        if (cookie != 0)
            operation->Unadvise(cookie);
        sink->Release();
        operation->Release();
        if (FAILED(hr) || aborted) {
            HRESULT reported = FAILED(hr) ? hr : HRESULT_FROM_WIN32(ERROR_CANCELLED);
            if (paths.size() == 1 && deleteAccessDenied(paths.first()))
                reported = HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
            return failure(reported,
                           QStringLiteral("Windows could not move the selected item to the Recycle Bin."));
        }
        const QStringList afterEntries = recycleEntryTokens();
        QStringList added;
        for (const QString &token : afterEntries) {
            if (!before.contains(token))
                added.append(token);
        }
        // A concurrent Recycle Bin change makes the snapshot ambiguous. The
        // delete still succeeded, but declining to advertise Undo is safer than
        // restoring somebody else's item.
        return PlatformResult::success(added.size() == paths.size() ? added : QStringList{});
    }

    PlatformResult restoreFromTrash(const QStringList &entries) override {
        ComScope com;
        if (FAILED(com.result) && com.result != RPC_E_CHANGED_MODE)
            return failure(com.result, QStringLiteral("COM initialization failed."));
        for (const QString &entry : entries) {
            const HRESULT hr = restoreEntry(entry);
            if (FAILED(hr))
                return failure(hr, QStringLiteral("Windows could not restore the selected item."));
        }
        return PlatformResult::success();
    }

private:
    static PlatformResult failure(HRESULT hr, const QString &message) {
        // Decode to a plain Win32 error code (not the raw HRESULT): callers classify
        // nativeCode against ERROR_ACCESS_DENIED and friends, which only matches the
        // low-order Win32 code, not the HRESULT bit pattern (e.g. 0x80070005).
        qint64 nativeCode = static_cast<qint64>(hr);
        if (HRESULT_FACILITY(hr) == FACILITY_WIN32)
            nativeCode = static_cast<qint64>(HRESULT_CODE(hr));
        else if (hr == E_ACCESSDENIED)
            nativeCode = ERROR_ACCESS_DENIED;
        const PlatformError code = nativeCode == ERROR_ACCESS_DENIED
            ? PlatformError::PermissionDenied
            : PlatformError::NativeFailure;
        return PlatformResult::failure(code, message, nativeCode);
    }
};
}

std::unique_ptr<TrashService> createTrashService() {
    return std::make_unique<WindowsTrashService>();
}
