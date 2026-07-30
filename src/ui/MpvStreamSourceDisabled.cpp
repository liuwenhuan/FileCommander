#include "MpvStreamSource.h"

#include <atomic>

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
