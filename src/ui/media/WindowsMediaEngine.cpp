#include "WindowsMediaEngine.h"

#include "WindowsMediaSurface.h"
#include "FileProvider.h"

#include <QFileInfo>
#include <QMetaObject>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <cmath>

#include <mfapi.h>
#include <mferror.h>
#include <mfmediaengine.h>
#include <oleauto.h>
#include <wincodec.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace {

constexpr int kPollIntervalMs = 33;

bool differs(double left, double right) {
    return std::abs(left - right) > 0.001;
}

QString hresultText(HRESULT hr) {
    return QStringLiteral("0x%1").arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
}

QString localFileUrlForMediaFoundation(const QString &path) {
    // IMFMediaEngine receives a UTF-16 BSTR and handles spaces/Unicode in local
    // file URLs. Passing a percent-encoded URL makes some valid Windows paths
    // fail to load, notably names containing brackets plus non-ASCII punctuation.
    return QUrl::fromLocalFile(path).toString(QUrl::None);
}

HRESULT ensureMediaFoundationStarted() {
    // IMFMediaEngine::Shutdown releases each engine's playback resources. Keep
    // Media Foundation's process-global runtime alive after the first startup:
    // on some Windows 11 + Intel media stacks, calling MFShutdown during
    // short-lived frame-server previews corrupts the process heap at exit.
    static const HRESULT hr = MFStartup(MF_VERSION);
    return hr;
}

QImage imageFromWicBitmap(IWICBitmap *bitmap, int width, int height) {
    if (!bitmap || width <= 0 || height <= 0)
        return {};

    WICRect rect{0, 0, width, height};
    ComPtr<IWICBitmapLock> lock;
    if (FAILED(bitmap->Lock(&rect, WICBitmapLockRead, &lock)))
        return {};

    UINT stride = 0;
    UINT size = 0;
    BYTE *data = nullptr;
    if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &data)) ||
        !data) {
        return {};
    }

    QImage image(width, height, QImage::Format_ARGB32);
    const int bytesPerLine = std::min<int>(image.bytesPerLine(), static_cast<int>(stride));
    for (int y = 0; y < height; ++y) {
        memcpy(image.scanLine(y), data + y * stride, bytesPerLine);
    }
    return image;
}

} // namespace

class WindowsMediaEngine::Notify final : public IMFMediaEngineNotify {
public:
    explicit Notify(WindowsMediaEngine *owner) : m_owner(owner) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
        if (!object)
            return E_POINTER;
        if (iid == IID_IUnknown || iid == __uuidof(IMFMediaEngineNotify)) {
            *object = static_cast<IMFMediaEngineNotify *>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&m_refs));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refs = static_cast<ULONG>(InterlockedDecrement(&m_refs));
        return refs;
    }

    HRESULT STDMETHODCALLTYPE EventNotify(DWORD event, DWORD_PTR param1, DWORD param2) override {
        if (event == MF_MEDIA_ENGINE_EVENT_NOTIFYSTABLESTATE) {
            if (param1)
                SetEvent(reinterpret_cast<HANDLE>(param1));
            return S_OK;
        }
        if (m_owner) {
            QMetaObject::invokeMethod(m_owner, [owner = QPointer<WindowsMediaEngine>(m_owner),
                                                event, param1, param2]() {
                if (owner)
                    owner->onMediaEvent(event, static_cast<quint64>(param1), param2);
            }, Qt::QueuedConnection);
        }
        return S_OK;
    }

    void detach() { m_owner = nullptr; }

private:
    LONG m_refs = 1;
    WindowsMediaEngine *m_owner = nullptr;
};

WindowsMediaEngine::WindowsMediaEngine(QObject *parent)
    : MediaEngine(parent), m_notify(std::make_unique<Notify>(this)),
      m_surface(new WindowsMediaSurface) {
    m_clock.start();
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(kPollIntervalMs);
    connect(m_pollTimer, &QTimer::timeout, this, [this]() {
        updateTimeline();
        updateVideoSize();
        pumpFrame();
        if (m_seekWatchdog.observe(m_position, m_clock.elapsed()) == SeekWatchdog::Verdict::Stuck)
            recoverFromStuckSeek();
    });
}

WindowsMediaEngine::~WindowsMediaEngine() {
    if (m_pollTimer)
        m_pollTimer->stop();
    if (m_notify)
        m_notify->detach();
    if (m_engine) {
        m_engine->Shutdown();
        m_engine->Release();
        m_engine = nullptr;
    }
    if (m_wic) {
        m_wic->Release();
        m_wic = nullptr;
    }
    if (m_comInitialized)
        CoUninitialize();
    if (m_surface && !m_surface->parent())
        delete m_surface.data();
    m_surface.clear();
}

void WindowsMediaEngine::initialize() {
    if (m_initialized)
        return;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr)) {
        m_comInitialized = true;
    } else if (hr != RPC_E_CHANGED_MODE) {
        throw std::runtime_error(("could not initialize COM: " + hresultText(hr)).toStdString());
    }

    hr = ensureMediaFoundationStarted();
    if (FAILED(hr))
        throw std::runtime_error(("could not start Media Foundation: " + hresultText(hr)).toStdString());

    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&m_wic));
    if (FAILED(hr))
        throw std::runtime_error(("could not create WIC factory: " + hresultText(hr)).toStdString());

    ComPtr<IMFMediaEngineClassFactory> factory;
    hr = CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&factory));
    if (FAILED(hr))
        throw std::runtime_error(("could not create Media Foundation engine factory: " +
                                  hresultText(hr)).toStdString());

    ComPtr<IMFAttributes> attributes;
    hr = MFCreateAttributes(&attributes, 4);
    if (FAILED(hr))
        throw std::runtime_error(("could not create Media Foundation attributes: " +
                                  hresultText(hr)).toStdString());

    attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, m_notify.get());
    attributes->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);

    hr = factory->CreateInstance(MF_MEDIA_ENGINE_WAITFORSTABLE_STATE, attributes.Get(),
                                 &m_engine);
    if (FAILED(hr))
        throw std::runtime_error(("could not create Media Foundation engine: " +
                                  hresultText(hr)).toStdString());

    m_engine->SetMuted(m_muted ? TRUE : FALSE);
    m_engine->SetVolume(qBound(0.0, m_volume / 100.0, 1.0));
    m_initialized = true;
    m_pollTimer->start();
}

void WindowsMediaEngine::load(const MediaSource &source, MediaKind kind) {
    m_source = source;
    m_kind = kind;
    ++m_loadGeneration;
    clearObservedValues();

    const QString url = sourceUrlForMediaFoundation(source);
    if (url.isEmpty()) {
        if (source.isRemote && source.provider &&
            source.provider->scheme() == QStringLiteral("webdav")) {
            setFailure(QStringLiteral("Windows WebDAV media preview requires a completed local cache for %1.")
                           .arg(source.path.isEmpty() ? QStringLiteral("<empty>") : source.path));
        } else {
            setFailure(QStringLiteral("Windows media preview requires a local or UNC media file: %1")
                           .arg(source.path.isEmpty() ? QStringLiteral("<empty>") : source.path));
        }
        return;
    }

    try {
        initialize();
    } catch (const std::exception &error) {
        setFailure(QString::fromUtf8(error.what()));
        return;
    }

    setState(MediaState::Loading);
    const std::wstring wideUrl = url.toStdWString();
    BSTR sourceUrl =
        SysAllocStringLen(wideUrl.data(), static_cast<UINT>(wideUrl.size()));
    const HRESULT hr = sourceUrl ? m_engine->SetSource(sourceUrl) : E_OUTOFMEMORY;
    if (sourceUrl)
        SysFreeString(sourceUrl);
    if (FAILED(hr)) {
        setFailure(QStringLiteral("Could not open media with Media Foundation: %1")
                       .arg(hresultText(hr)));
        return;
    }
    m_engine->SetPlaybackRate(kind == MediaKind::Audio ? 1.0 : m_speed);
    m_engine->Play();
}

void WindowsMediaEngine::stop() {
    if (m_engine)
        m_engine->Pause();
    m_source = {};
    clearObservedValues();
    setState(MediaState::Idle);
}

void WindowsMediaEngine::playPause() {
    if (!m_engine || m_source.path.isEmpty())
        return;
    if (ended()) {
        const MediaSource source = m_source;
        const MediaKind kind = m_kind;
        load(source, kind);
        return;
    }
    if (paused()) {
        m_engine->Play();
        setState(MediaState::Playing);
    } else {
        m_engine->Pause();
        setState(MediaState::Paused);
    }
}

void WindowsMediaEngine::seekFraction(double fraction) {
    if (!m_engine || m_duration <= 0.0)
        return;
    const double position = qBound(0.0, fraction, 1.0) * m_duration;
    if (SUCCEEDED(m_engine->SetCurrentTime(position))) {
        m_position = position;
        // Watch it: SetCurrentTime succeeding says only that the request was
        // taken, not that it will ever finish. See SeekWatchdog.
        m_seekWatchdog.arm(position, m_duration, m_state == MediaState::Playing,
                           m_clock.elapsed());
        emit positionChanged(m_position);
    }
}

void WindowsMediaEngine::recoverFromStuckSeek() {
    // The engine is past saving at this point -- it ignores further seeks, and
    // pausing or playing does not shake it loose (both measured). Reloading is
    // the only way back to a live picture, and it starts from the beginning
    // because seeking to where we were is the very thing that failed.
    const MediaSource source = m_source;
    const MediaKind kind = m_kind;
    if (source.path.isEmpty())
        return;
    load(source, kind);
    emit seekUnsupported();
}

void WindowsMediaEngine::setVolume(int volume) {
    m_volume = qBound(0, volume, 100);
    if (m_engine)
        m_engine->SetVolume(qBound(0.0, m_volume / 100.0, 1.0));
}

void WindowsMediaEngine::setMute(bool mute) {
    m_muted = mute;
    if (m_engine)
        m_engine->SetMuted(muted() ? TRUE : FALSE);
}

void WindowsMediaEngine::setSpeed(double speed) {
    m_speed = speed > 0.0 ? speed : 1.0;
    if (m_engine && m_kind == MediaKind::Video)
        m_engine->SetPlaybackRate(m_speed);
}

void WindowsMediaEngine::setVideoEffect(const VideoEffectSettings &settings) {
    if (m_surface)
        m_surface->setVideoEffect(settings);
}

void WindowsMediaEngine::setVideoRotation(int degrees) {
    // Remembered even with no surface yet: load() builds one, and the viewer's
    // choice should survive that rather than silently reset.
    m_rotation = ((degrees % 360) + 360) % 360;
    if (m_surface)
        m_surface->setRotation(m_rotation);
}

QWidget *WindowsMediaEngine::videoSurface() { return m_surface; }

MediaState WindowsMediaEngine::state() const { return m_state; }
MediaKind WindowsMediaEngine::currentKind() const { return m_kind; }
MediaSource WindowsMediaEngine::currentSource() const { return m_source; }
double WindowsMediaEngine::durationSeconds() const { return m_duration; }
double WindowsMediaEngine::positionSeconds() const { return m_position; }
bool WindowsMediaEngine::paused() const { return m_state == MediaState::Paused; }
bool WindowsMediaEngine::ended() const { return m_state == MediaState::Ended; }
int WindowsMediaEngine::volume() const { return m_volume; }
bool WindowsMediaEngine::muted() const { return m_muted; }
QSize WindowsMediaEngine::currentVideoSize() const { return m_videoSize; }
QString WindowsMediaEngine::videoCodec() const { return m_videoCodec; }

void WindowsMediaEngine::setState(MediaState state) {
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged(m_state);
}

void WindowsMediaEngine::setFailure(const QString &message) {
    if (m_engine)
        m_engine->Pause();
    setState(MediaState::Failed);
    emit errorOccurred(message);
}

void WindowsMediaEngine::clearObservedValues() {
    m_seekWatchdog.disarm();
    m_duration = 0.0;
    m_position = 0.0;
    m_videoSize = {};
    m_videoCodec.clear();
    m_sawVideoFrame = false;
    emit durationChanged(0.0);
    emit positionChanged(0.0);
    emit videoSizeChanged({});
}

void WindowsMediaEngine::updateTimeline() {
    if (!m_engine || m_state == MediaState::Idle || m_state == MediaState::Failed)
        return;
    const double duration = m_engine->GetDuration();
    if (std::isfinite(duration) && differs(duration, m_duration)) {
        m_duration = duration;
        emit durationChanged(m_duration);
    }
    const double position = m_engine->GetCurrentTime();
    if (std::isfinite(position) && differs(position, m_position)) {
        m_position = position;
        emit positionChanged(m_position);
    }
}

void WindowsMediaEngine::updateVideoSize() {
    if (!m_engine || m_kind != MediaKind::Video)
        return;
    DWORD width = 0;
    DWORD height = 0;
    if (FAILED(m_engine->GetNativeVideoSize(&width, &height)) || width == 0 || height == 0)
        return;
    const QSize size(static_cast<int>(width), static_cast<int>(height));
    if (size == m_videoSize)
        return;
    m_videoSize = size;
    emit videoSizeChanged(m_videoSize);
}

void WindowsMediaEngine::pumpFrame() {
    if (!m_engine || !m_wic || m_kind != MediaKind::Video || !m_surface)
        return;
    LONGLONG pts = 0;
    const HRESULT tick = m_engine->OnVideoStreamTick(&pts);
    if (tick != S_OK)
        return;
    if (m_videoSize.isEmpty())
        updateVideoSize();
    if (m_videoSize.isEmpty())
        return;

    ComPtr<IWICBitmap> bitmap;
    HRESULT hr = m_wic->CreateBitmap(static_cast<UINT>(m_videoSize.width()),
                                     static_cast<UINT>(m_videoSize.height()),
                                     GUID_WICPixelFormat32bppBGRA,
                                     WICBitmapCacheOnLoad, &bitmap);
    if (FAILED(hr))
        return;

    MFVideoNormalizedRect sourceRect{0.0f, 0.0f, 1.0f, 1.0f};
    RECT destination{0, 0, m_videoSize.width(), m_videoSize.height()};
    MFARGB border{255, 0, 0, 0};
    hr = m_engine->TransferVideoFrame(bitmap.Get(), &sourceRect, &destination, &border);
    if (FAILED(hr))
        return;

    const QImage frame = imageFromWicBitmap(bitmap.Get(), m_videoSize.width(), m_videoSize.height());
    if (!frame.isNull()) {
        m_sawVideoFrame = true;
        m_surface->setFrame(frame);
    }
}

void WindowsMediaEngine::onMediaEvent(unsigned long event, quint64 param1, unsigned long param2) {
    switch (event) {
    case MF_MEDIA_ENGINE_EVENT_LOADSTART:
        setState(MediaState::Loading);
        break;
    case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA:
    case MF_MEDIA_ENGINE_EVENT_DURATIONCHANGE:
        updateTimeline();
        updateVideoSize();
        break;
    case MF_MEDIA_ENGINE_EVENT_FIRSTFRAMEREADY:
        updateTimeline();
        updateVideoSize();
        pumpFrame();
        break;
    case MF_MEDIA_ENGINE_EVENT_PLAYING:
    case MF_MEDIA_ENGINE_EVENT_CANPLAY:
        setState(MediaState::Playing);
        updateTimeline();
        break;
    case MF_MEDIA_ENGINE_EVENT_PAUSE:
        if (!ended())
            setState(MediaState::Paused);
        break;
    case MF_MEDIA_ENGINE_EVENT_ENDED:
        setState(MediaState::Ended);
        break;
    case MF_MEDIA_ENGINE_EVENT_ABORT:
        break;
    case MF_MEDIA_ENGINE_EVENT_ERROR:
        setFailure(errorText(param1, param2, m_source.path));
        break;
    case MF_MEDIA_ENGINE_EVENT_STREAMRENDERINGERROR: {
        const QString message =
            QStringLiteral("Could not play media with Media Foundation. "
                           "Stream rendering error event=1014 param1=0x%1 param2=0x%2")
                .arg(param1, 0, 16)
                .arg(param2, 0, 16);
        if (m_kind != MediaKind::Video) {
            setFailure(message);
            break;
        }
        if (m_sawVideoFrame)
            break;
        const quint64 generation = m_loadGeneration;
        QTimer::singleShot(1000, this, [this, generation, message]() {
            if (generation == m_loadGeneration && m_kind == MediaKind::Video &&
                m_state != MediaState::Failed && !m_sawVideoFrame) {
                updateTimeline();
                updateVideoSize();
                pumpFrame();
                if (!m_sawVideoFrame)
                    setFailure(message);
            }
        });
        break;
    }
    default:
        break;
    }
}

QString WindowsMediaEngine::errorText(quint64 code, unsigned long extended, const QString &path) {
    // The codes are the HTML5 MediaError values Media Foundation reuses. The
    // one that matters in practice is SRC_NOT_SUPPORTED: Windows ships no
    // decoder for several formats that are still common in the wild -- the
    // file that prompted this holds MPEG-2 video in an AVI container, which
    // ffmpeg decodes and Media Foundation will not touch -- and the old
    // message reported that as a pair of hex numbers, which told the user
    // nothing.
    const QString suffix = QStringLiteral(" [%1/0x%2]")
                               .arg(code)
                               .arg(static_cast<quint32>(extended), 8, 16, QLatin1Char('0'));
    const QString extension = QFileInfo(path).suffix().toLower();
    QString reason;
    switch (code) {
    case MF_MEDIA_ENGINE_ERR_ABORTED:
        reason = tr("Playback was stopped before it began.");
        break;
    case MF_MEDIA_ENGINE_ERR_NETWORK:
        reason = tr("The file could not be read to the end.");
        break;
    case MF_MEDIA_ENGINE_ERR_DECODE:
        reason = tr("The stream could not be decoded — the file may be damaged.");
        break;
    case MF_MEDIA_ENGINE_ERR_SRC_NOT_SUPPORTED:
        reason = extension.isEmpty()
                     ? tr("Windows has no decoder for this file's format.")
                     : tr("Windows has no decoder for this .%1 file's format. MPEG-2, "
                          "Xvid and DivX video are common in older files and Windows "
                          "carries none of them for this container; installing a codec "
                          "pack lets it play.")
                           .arg(extension);
        break;
    case MF_MEDIA_ENGINE_ERR_ENCRYPTED:
        reason = tr("The file is protected and cannot be played here.");
        break;
    default:
        reason = tr("Media Foundation could not play this file.");
        break;
    }
    return reason + suffix;
}

QString WindowsMediaEngine::sourceUrlForMediaFoundation(const MediaSource &source) {
    if (source.path.isEmpty())
        return {};
    if (!source.isRemote)
        return localFileUrlForMediaFoundation(source.path);
    if (!source.provider || source.provider->scheme() != QStringLiteral("webdav"))
        return {};
    if (source.localCachePath.isEmpty())
        return {};
    const QFileInfo cache(source.localCachePath);
    if (!cache.exists() || cache.isDir())
        return {};
    return localFileUrlForMediaFoundation(cache.absoluteFilePath());
}
