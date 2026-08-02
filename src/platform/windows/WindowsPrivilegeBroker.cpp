#include "privilege/PrivilegeBroker.h"

#include <QDir>
#include <QFileInfo>

#include <atomic>
#include <utility>

#include <windows.h>
#include <shobjidl.h>

namespace {

PrivilegeBroker::WindowsPrivilegeExecutor &testExecutor()
{
    static PrivilegeBroker::WindowsPrivilegeExecutor executor;
    return executor;
}

qint64 nativeCode(HRESULT result)
{
    if (HRESULT_FACILITY(result) == FACILITY_WIN32)
        return static_cast<qint64>(HRESULT_CODE(result));
    if (result == E_ACCESSDENIED)
        return ERROR_ACCESS_DENIED;
    return static_cast<qint64>(result);
}

PrivilegeResult resultFromHresult(HRESULT result, const QString &message)
{
    const qint64 code = nativeCode(result);
    if (code == ERROR_CANCELLED)
        return {PrivilegeStatus::Cancelled, code, message};
    if (code == ERROR_ACCESS_DENIED)
        return {PrivilegeStatus::Denied, code, message};
    return {PrivilegeStatus::Failed, code, message};
}

class ComScope {
public:
    ComScope() : m_result(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComScope()
    {
        if (SUCCEEDED(m_result))
            CoUninitialize();
    }

    HRESULT result() const { return m_result; }

private:
    HRESULT m_result;
};

class CancellationSink final : public IFileOperationProgressSink {
public:
    explicit CancellationSink(PrivilegeBroker::CancelCheck cancelled)
        : m_cancelled(std::move(cancelled)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override
    {
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
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG references = --m_references;
        if (references == 0)
            delete this;
        return references;
    }

    HRESULT STDMETHODCALLTYPE StartOperations() override { return checkCancelled(); }
    HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT result) override
    {
        remember(result);
        return checkCancelled();
    }
    HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD, IShellItem *, LPCWSTR) override
    { return checkCancelled(); }
    HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD, IShellItem *, LPCWSTR, HRESULT result,
                                              IShellItem *) override
    { remember(result); return checkCancelled(); }
    HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD, IShellItem *, IShellItem *, LPCWSTR) override
    { return checkCancelled(); }
    HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD, IShellItem *, IShellItem *, LPCWSTR,
                                            HRESULT result, IShellItem *) override
    { remember(result); return checkCancelled(); }
    HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD, IShellItem *, IShellItem *, LPCWSTR) override
    { return checkCancelled(); }
    HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD, IShellItem *, IShellItem *, LPCWSTR,
                                            HRESULT result, IShellItem *) override
    { remember(result); return checkCancelled(); }
    HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD, IShellItem *) override
    { return checkCancelled(); }
    HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD, IShellItem *, HRESULT result,
                                              IShellItem *) override
    { remember(result); return checkCancelled(); }
    HRESULT STDMETHODCALLTYPE PreNewItem(DWORD, IShellItem *, LPCWSTR) override
    { return checkCancelled(); }
    HRESULT STDMETHODCALLTYPE PostNewItem(DWORD, IShellItem *, LPCWSTR, LPCWSTR, DWORD,
                                           HRESULT result, IShellItem *) override
    { remember(result); return checkCancelled(); }
    HRESULT STDMETHODCALLTYPE UpdateProgress(UINT, UINT) override { return checkCancelled(); }
    HRESULT STDMETHODCALLTYPE ResetTimer() override { return checkCancelled(); }
    HRESULT STDMETHODCALLTYPE PauseTimer() override { return checkCancelled(); }
    HRESULT STDMETHODCALLTYPE ResumeTimer() override { return checkCancelled(); }

    HRESULT operationResult() const { return m_operationResult; }

private:
    HRESULT checkCancelled() const
    {
        return m_cancelled && m_cancelled()
            ? HRESULT_FROM_WIN32(ERROR_CANCELLED)
            : S_OK;
    }

    void remember(HRESULT result)
    {
        if (FAILED(result) && SUCCEEDED(m_operationResult))
            m_operationResult = result;
    }

    std::atomic<ULONG> m_references{1};
    PrivilegeBroker::CancelCheck m_cancelled;
    HRESULT m_operationResult = S_OK;
};

HRESULT shellItem(const QString &path, IShellItem **item)
{
    const QString native = QDir::toNativeSeparators(path);
    return SHCreateItemFromParsingName(reinterpret_cast<LPCWSTR>(native.utf16()), nullptr,
                                       IID_PPV_ARGS(item));
}

HRESULT queueOperation(IFileOperation *operation, const PrivilegedOperationRequest &request)
{
    if ((QFileInfo::exists(request.targetPath) || QFileInfo(request.targetPath).isSymLink()) &&
        !request.overwrite && request.kind != PrivilegedOperationKind::DeletePermanent &&
        request.kind != PrivilegedOperationKind::Rename) {
        return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    }

    IShellItem *source = nullptr;
    IShellItem *destinationFolder = nullptr;
    HRESULT result = S_OK;
    if (!request.sourcePath.isEmpty())
        result = shellItem(request.sourcePath, &source);

    const QFileInfo targetInfo(request.targetPath);
    if (SUCCEEDED(result) && !request.targetPath.isEmpty() &&
        request.kind != PrivilegedOperationKind::DeletePermanent) {
        result = shellItem(targetInfo.absolutePath(), &destinationFolder);
    }

    if (SUCCEEDED(result)) {
        const QString targetFileName = targetInfo.fileName();
        const wchar_t *targetName = reinterpret_cast<const wchar_t *>(targetFileName.utf16());
        switch (request.kind) {
        case PrivilegedOperationKind::Copy:
            result = operation->CopyItem(source, destinationFolder, targetName, nullptr);
            break;
        case PrivilegedOperationKind::Move:
            result = operation->MoveItem(source, destinationFolder, targetName, nullptr);
            break;
        case PrivilegedOperationKind::DeletePermanent:
            result = operation->DeleteItem(source, nullptr);
            break;
        case PrivilegedOperationKind::Mkdir:
            result = operation->NewItem(destinationFolder, FILE_ATTRIBUTE_DIRECTORY,
                                        targetName, nullptr, nullptr);
            break;
        case PrivilegedOperationKind::Rename:
            if (QFileInfo(request.sourcePath).absolutePath() == targetInfo.absolutePath())
                result = operation->RenameItem(source, targetName, nullptr);
            else
                result = operation->MoveItem(source, destinationFolder, targetName, nullptr);
            break;
        case PrivilegedOperationKind::Symlink:
            result = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
            break;
        }
    }

    if (destinationFolder)
        destinationFolder->Release();
    if (source)
        source->Release();
    return result;
}

PrivilegeResult executeWithWindowsShell(const PrivilegedOperationRequest &request,
                                        const PrivilegeBroker::CancelCheck &cancelled)
{
    if (cancelled && cancelled())
        return {PrivilegeStatus::Cancelled, ERROR_CANCELLED, QStringLiteral("The operation was cancelled.")};

    ComScope com;
    if (FAILED(com.result()) && com.result() != RPC_E_CHANGED_MODE)
        return resultFromHresult(com.result(), QStringLiteral("COM initialization failed."));

    // CoCreateInstance(CLSID_FileOperation, ..., CLSCTX_INPROC_SERVER, ...) builds the
    // operation object inside this (non-elevated) process, so FOFX_REQUIREELEVATION has
    // no separate process to elevate: no UAC prompt appears and the operation silently
    // runs (and fails) at the caller's own integrity level. Requesting the object through
    // the Shell's elevation moniker instead creates it out-of-process under an elevated
    // token, which is what actually triggers the UAC consent prompt.
    BIND_OPTS3 bindOptions;
    ZeroMemory(&bindOptions, sizeof(bindOptions));
    bindOptions.cbStruct = sizeof(bindOptions);
    bindOptions.dwClassContext = CLSCTX_LOCAL_SERVER;
    // BIND_OPTS3::hwnd is the owner window the elevation subsystem parents the UAC consent
    // prompt to; leaving it null was the actual reason the prompt surfaced only as a
    // background/taskbar-flashing window instead of being brought to front, regardless of
    // AllowSetForegroundWindow below. This call runs on a worker thread with no Qt widget
    // access, so it asks Win32 directly for whichever of our own top-level windows
    // currently owns the foreground (the dialog the user just clicked "Run as
    // administrator" in, or its parent once that dialog has closed).
    bindOptions.hwnd = GetForegroundWindow();

    // The UAC consent prompt is shown by a separate system process (consent.exe) spawned
    // during this CoGetObject call, under Windows' usual foreground-lock: a background
    // process normally cannot bring its own window to the front, so without this the
    // prompt appears behind FileCommander (just a flashing taskbar entry) instead of
    // grabbing focus. ASFW_ANY grants the next process that asks -- consent.exe -- one-time
    // permission to foreground itself.
    AllowSetForegroundWindow(ASFW_ANY);

    IFileOperation *operation = nullptr;
    HRESULT result = CoGetObject(
        L"Elevation:Administrator!new:{3AD05575-8857-4850-9277-11B85BDB8E09}",
        &bindOptions, IID_PPV_ARGS(&operation));
    if (FAILED(result))
        return resultFromHresult(result, QStringLiteral("Windows file operations are unavailable."));

    const FILEOP_FLAGS flags = static_cast<FILEOP_FLAGS>(
        FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT |
        FOFX_SHOWELEVATIONPROMPT | FOFX_REQUIREELEVATION | FOFX_EARLYFAILURE);
    result = operation->SetOperationFlags(flags);

    auto *sink = new CancellationSink(cancelled);
    DWORD cookie = 0;
    if (SUCCEEDED(result))
        result = operation->Advise(sink, &cookie);
    if (SUCCEEDED(result))
        result = queueOperation(operation, request);
    if (SUCCEEDED(result))
        result = operation->PerformOperations();

    BOOL aborted = FALSE;
    if (SUCCEEDED(result))
        operation->GetAnyOperationsAborted(&aborted);
    if (SUCCEEDED(result) && FAILED(sink->operationResult()))
        result = sink->operationResult();
    if (SUCCEEDED(result) && aborted)
        result = HRESULT_FROM_WIN32(ERROR_CANCELLED);

    if (cookie != 0)
        operation->Unadvise(cookie);
    sink->Release();
    operation->Release();

    if (FAILED(result))
        return resultFromHresult(result, QStringLiteral("Windows did not complete the administrator operation."));
    return {PrivilegeStatus::Succeeded, ERROR_SUCCESS, {}};
}

} // namespace

namespace PrivilegeBroker {

bool isAvailable()
{
    return true;
}

PrivilegeResult execute(const PrivilegedOperationRequest &request, CancelCheck cancelled)
{
    const PrivilegeResult validation = validatePrivilegedOperationRequest(request);
    if (validation.status != PrivilegeStatus::Succeeded)
        return validation;
    if (testExecutor())
        return testExecutor()(request, cancelled);
    return executeWithWindowsShell(request, cancelled);
}

void setWindowsPrivilegeExecutorForTesting(WindowsPrivilegeExecutor executor)
{
    testExecutor() = std::move(executor);
}

void resetWindowsPrivilegeExecutorForTesting()
{
    testExecutor() = {};
}

} // namespace PrivilegeBroker
