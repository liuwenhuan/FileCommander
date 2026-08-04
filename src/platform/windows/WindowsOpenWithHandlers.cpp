#include "OpenWithHandlers.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>

#include <windows.h>

#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace fc {

namespace {

// COM has to be live on this thread for the shell interfaces. The GUI thread
// already initialises it, so this is normally a no-op that records it did
// nothing; it exists so the enumeration also works from a test or a worker.
class ComScope {
public:
    ComScope() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        m_owned = SUCCEEDED(hr);
    }
    ~ComScope() {
        if (m_owned)
            CoUninitialize();
    }
    ComScope(const ComScope &) = delete;
    ComScope &operator=(const ComScope &) = delete;

private:
    bool m_owned = false;
};

QString fromCoTaskMem(LPWSTR text) {
    if (!text)
        return {};
    const QString value = QString::fromWCharArray(text);
    CoTaskMemFree(text);
    return value;
}

// The shell reports an icon as "file,index", where the file may itself be a
// resource-only DLL. Split it so callers can hand the pair to whatever loads
// icons on their side.
void splitIconLocation(const QString &location, QString *path, int *index) {
    *path = location;
    *index = 0;
    const int comma = location.lastIndexOf(QLatin1Char(','));
    if (comma <= 0)
        return;
    bool ok = false;
    const int parsed = location.mid(comma + 1).toInt(&ok);
    if (!ok)
        return;
    *path = location.left(comma);
    *index = parsed;
}

ComPtr<IDataObject> dataObjectFor(const QString &filePath) {
    ComPtr<IShellItem> item;
    const std::wstring wide = QDir::toNativeSeparators(filePath).toStdWString();
    if (FAILED(SHCreateItemFromParsingName(wide.c_str(), nullptr, IID_PPV_ARGS(&item))) || !item)
        return {};
    ComPtr<IDataObject> object;
    if (FAILED(item->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&object))))
        return {};
    return object;
}

void collect(const QString &extension, ASSOC_FILTER filter, bool recommended,
             QVector<OpenWithHandler> *out) {
    const std::wstring wide = extension.toStdWString();
    ComPtr<IEnumAssocHandlers> handlers;
    if (FAILED(SHAssocEnumHandlers(wide.c_str(), filter, &handlers)) || !handlers)
        return;

    for (;;) {
        ComPtr<IAssocHandler> handler;
        ULONG fetched = 0;
        if (handlers->Next(1, &handler, &fetched) != S_OK || fetched == 0 || !handler)
            break;

        OpenWithHandler entry;
        LPWSTR text = nullptr;
        if (SUCCEEDED(handler->GetUIName(&text)))
            entry.displayName = fromCoTaskMem(text);
        text = nullptr;
        if (SUCCEEDED(handler->GetName(&text))) {
            // GetName is the launch target: an executable path for a desktop
            // application, an opaque activation string for a Store app.
            const QString name = fromCoTaskMem(text);
            if (QFileInfo(name).isAbsolute() && QFileInfo::exists(name))
                entry.program = name;
            entry.token = name;
        }
        if (entry.displayName.isEmpty())
            entry.displayName = QFileInfo(entry.program).completeBaseName();
        text = nullptr;
        int iconIndex = 0;
        if (SUCCEEDED(handler->GetIconLocation(&text, &iconIndex))) {
            QString location = fromCoTaskMem(text);
            splitIconLocation(location, &location, &iconIndex);
            entry.iconPath = location;
            entry.iconIndex = iconIndex;
        }
        if (entry.iconPath.isEmpty())
            entry.iconPath = entry.program;
        entry.recommended = recommended || handler->IsRecommended() == S_OK;
        if (!entry.displayName.isEmpty() || !entry.program.isEmpty())
            out->append(entry);
    }
}

} // namespace

QVector<OpenWithHandler> openWithHandlers(const QString &filePath) {
    const QString suffix = QFileInfo(filePath).suffix();
    if (suffix.isEmpty())
        return {};

    ComScope com;
    const QString extension = QLatin1Char('.') + suffix;
    QVector<OpenWithHandler> found;
    // Two passes rather than one: RECOMMENDED is the list Explorer puts at the
    // top, and asking for it separately is the only way to learn which entries
    // of the full list belong there -- IsRecommended() answers per handler, but
    // a handler that only appears under the ALL filter never gets asked.
    collect(extension, ASSOC_FILTER_RECOMMENDED, true, &found);
    collect(extension, ASSOC_FILTER_NONE, false, &found);
    return tidyOpenWithHandlers(std::move(found));
}

bool launchOpenWithHandler(const OpenWithHandler &handler, const QString &filePath) {
    ComScope com;
    const QString suffix = QFileInfo(filePath).suffix();
    if (!handler.token.isEmpty() && !suffix.isEmpty()) {
        // Ask the shell to invoke the registration itself. This is what makes
        // a Store app work -- it has no command line to run -- and it also
        // honours whatever DDE or verb the registration carries.
        const QString extension = QLatin1Char('.') + suffix;
        const std::wstring wideExtension = extension.toStdWString();
        ComPtr<IEnumAssocHandlers> handlers;
        if (SUCCEEDED(SHAssocEnumHandlers(wideExtension.c_str(), ASSOC_FILTER_NONE, &handlers)) &&
            handlers) {
            for (;;) {
                ComPtr<IAssocHandler> candidate;
                ULONG fetched = 0;
                if (handlers->Next(1, &candidate, &fetched) != S_OK || fetched == 0 || !candidate)
                    break;
                LPWSTR text = nullptr;
                if (FAILED(candidate->GetName(&text)))
                    continue;
                if (fromCoTaskMem(text).compare(handler.token, Qt::CaseInsensitive) != 0)
                    continue;
                ComPtr<IDataObject> object = dataObjectFor(filePath);
                if (!object)
                    break;
                ComPtr<IAssocHandlerInvoker> invoker;
                if (FAILED(candidate->CreateInvoker(object.Get(), &invoker)) || !invoker)
                    break;
                if (invoker->SupportsSelection() != S_OK)
                    break;
                return SUCCEEDED(invoker->Invoke());
            }
        }
    }

    if (handler.program.isEmpty())
        return false;
    QStringList arguments = handler.arguments;
    arguments.append(QDir::toNativeSeparators(filePath));
    return QProcess::startDetached(handler.program, arguments);
}

} // namespace fc
