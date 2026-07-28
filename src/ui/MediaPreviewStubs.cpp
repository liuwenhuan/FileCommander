#include "AudioPlayer.h"
#include "MpvWidget.h"
#include "MpvStreamSource.h"

AudioPlayer::AudioPlayer(QObject *parent) : QObject(parent) {}
AudioPlayer::~AudioPlayer() = default;
void AudioPlayer::load(const QString &) {}
void AudioPlayer::stop() {}
void AudioPlayer::playPause() {}
void AudioPlayer::seekFraction(double) {}
void AudioPlayer::setVolume(int) {}
void AudioPlayer::setMute(bool) {}
double AudioPlayer::durationSeconds() const { return 0.0; }
double AudioPlayer::positionSeconds() const { return 0.0; }
bool AudioPlayer::paused() const { return true; }
bool AudioPlayer::ended() const { return true; }
QString AudioPlayer::metadata(const QString &) const { return {}; }
void AudioPlayer::onMpvEvents() {}
double AudioPlayer::getDouble(const char *) const { return 0.0; }
bool AudioPlayer::getFlag(const char *) const { return false; }
void AudioPlayer::onWakeup(void *) {}

MpvWidget::MpvWidget(QWidget *parent) : QOpenGLWidget(parent) {}
MpvWidget::~MpvWidget() = default;
void MpvWidget::load(const QString &path) { emit loadFailed(path); }
void MpvWidget::stop() {}
void MpvWidget::playPause() {}
void MpvWidget::setSpeed(double) {}
void MpvWidget::seekFraction(double) {}
void MpvWidget::setVolume(int) {}
void MpvWidget::setMute(bool) {}
void MpvWidget::applyVideoFilter(const QString &) {}
double MpvWidget::durationSeconds() const { return 0.0; }
double MpvWidget::positionSeconds() const { return 0.0; }
bool MpvWidget::paused() const { return true; }
bool MpvWidget::eofReached() const { return false; }
bool MpvWidget::ended() const { return true; }
int MpvWidget::videoWidth() const { return 0; }
int MpvWidget::videoHeight() const { return 0; }
QString MpvWidget::videoCodec() const { return {}; }
void MpvWidget::doUpdate() {}
void MpvWidget::onMpvEvents() {}
void MpvWidget::initializeGL() {}
void MpvWidget::paintGL() {}
double MpvWidget::getDouble(const char *) const { return 0.0; }
long long MpvWidget::getInt(const char *) const { return 0; }
bool MpvWidget::getFlag(const char *) const { return false; }
void *MpvWidget::getProcAddress(void *, const char *) { return nullptr; }
void MpvWidget::onMpvRenderUpdate(void *) {}
void MpvWidget::onMpvWakeup(void *) {}

namespace MpvStreamSource {
namespace {
std::atomic<int> g_activeStreams{0};
}
const char *const kScheme = "fcstream";
bool registerProtocol(mpv_handle *) { return false; }
QString publish(const std::shared_ptr<FileProvider> &, const QString &) { return {}; }
bool isStreamUrl(const QString &) { return false; }
void revoke(const QString &) {}
int activeStreams() { return g_activeStreams.load(); }
Stream::Stream(std::shared_ptr<FileProvider> provider, QString remotePath)
    : m_provider(std::move(provider)), m_path(std::move(remotePath)) {
    ++g_activeStreams;
}
Stream::~Stream() { --g_activeStreams; }
} // namespace MpvStreamSource
