#include "TrashService.h"

#include <QDir>

#include <windows.h>
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

class WindowsTrashService final : public TrashService {
public:
    PlatformResult moveToTrash(const QStringList &paths) override {
        ComScope com;
        if (FAILED(com.result) && com.result != RPC_E_CHANGED_MODE)
            return failure(com.result, QStringLiteral("COM initialization failed."));
        IFileOperation *operation = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&operation));
        if (FAILED(hr))
            return failure(hr, QStringLiteral("Windows recycle service is unavailable."));
        operation->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMATION |
                                     FOF_NOERRORUI | FOF_SILENT);
        for (const QString &path : paths) {
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
        operation->Release();
        if (FAILED(hr) || aborted)
            return failure(FAILED(hr) ? hr : HRESULT_FROM_WIN32(ERROR_CANCELLED),
                           QStringLiteral("Windows could not move the selected item to the Recycle Bin."));
        return PlatformResult::success();
    }

private:
    static PlatformResult failure(HRESULT hr, const QString &message) {
        const PlatformError code =
            hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) ? PlatformError::PermissionDenied
                                                         : PlatformError::NativeFailure;
        return PlatformResult::failure(code, message, static_cast<qint64>(hr));
    }
};
}

std::unique_ptr<TrashService> createTrashService() {
    return std::make_unique<WindowsTrashService>();
}
