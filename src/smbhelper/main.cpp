// ttc-smb-helper: a minimal, single-threaded SMB read server.
//
// One instance owns exactly one libsmbclient context and serves read-only
// requests for it over stdin/stdout. The parent (SmbHelperClient) runs a small
// pool of these so thumbnailing a share reads several files at once -- which an
// in-process connection pool cannot do, because libsmbclient's global talloc /
// smb.conf state is corrupted by concurrent use even across separate contexts.
//
// Deliberately Qt-free and free of the rest of ttc: the whole point is that a
// libsmbclient abort takes down this process alone, so the parent must share as
// little code (and no address space) with it.
//
// Protocol: see src/core/network/SmbHelperProtocol.h.

#include "SmbHelperProtocol.h"

#include <libsmbclient.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

using namespace smbhelper;

namespace {

// Credentials, captured from the Hello frame and handed to libsmbclient's auth
// callback. File-scope because the callback is a C function pointer with no
// user-data parameter we need beyond the context; this process serves exactly
// one connection, so one set of credentials is all there ever is.
std::string g_user;
std::string g_password;
std::string g_workgroup;

void authCallback(SMBCCTX * /*ctx*/, const char * /*srv*/, const char * /*shr*/, char *wg,
                  int wglen, char *un, int unlen, char *pw, int pwlen) {
    auto copyField = [](char *dst, int cap, const std::string &src) {
        if (!dst || cap <= 0)
            return;
        const std::size_t n = src.size() < static_cast<std::size_t>(cap - 1)
                                  ? src.size()
                                  : static_cast<std::size_t>(cap - 1);
        std::memcpy(dst, src.data(), n);
        dst[n] = '\0';
    };
    copyField(wg, wglen, g_workgroup);
    copyField(un, unlen, g_user);
    copyField(pw, pwlen, g_password);
}

// Reads exactly n bytes, resuming across short reads and EINTR. Returns false
// on EOF or a hard error -- either way the parent is gone or the stream is
// desynchronised, and the only safe response is to exit.
bool readExact(int fd, void *buf, std::size_t n) {
    auto *p = static_cast<unsigned char *>(buf);
    std::size_t done = 0;
    while (done < n) {
        const ssize_t r = ::read(fd, p + done, n - done);
        if (r > 0) {
            done += static_cast<std::size_t>(r);
            continue;
        }
        if (r < 0 && errno == EINTR)
            continue;
        return false; // EOF (parent closed) or a real error
    }
    return true;
}

bool writeExact(int fd, const void *buf, std::size_t n) {
    const auto *p = static_cast<const unsigned char *>(buf);
    std::size_t done = 0;
    while (done < n) {
        const ssize_t w = ::write(fd, p + done, n - done);
        if (w > 0) {
            done += static_cast<std::size_t>(w);
            continue;
        }
        if (w < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

// Sends one reply frame. Header and payload go out in a single buffer so a
// reply is never split by a signal into a header the parent can't complete.
bool sendReply(Status status, const void *payload, std::uint32_t len) {
    std::vector<unsigned char> frame(kHeaderSize + len);
    writeHeader(frame.data(), len, static_cast<std::uint8_t>(status));
    if (len > 0)
        std::memcpy(frame.data() + kHeaderSize, payload, len);
    return writeExact(STDOUT_FILENO, frame.data(), frame.size());
}

bool sendError(const char *reason) {
    return sendReply(Status::Error, reason, static_cast<std::uint32_t>(std::strlen(reason)));
}

bool sendOk() { return sendReply(Status::Ok, nullptr, 0); }

// Open SMB files, keyed by the id handed back to the parent. Ids are never
// reused within a process, so a stale id from the parent fails cleanly instead
// of hitting an unrelated file.
struct HelperState {
    SMBCCTX *ctx = nullptr;
    std::unordered_map<std::uint64_t, SMBCFILE *> files;
    std::uint64_t nextId = 1;
};

// Parses the Hello payload and builds the one context this process will serve.
// Returns false with a reason on any failure; the parent then falls back to its
// own in-process path.
bool handleHello(HelperState &state, const std::vector<unsigned char> &payload, std::string *error) {
    // Four NUL-terminated fields, then the fixed tail. Every offset is bounds
    // checked against the frame the parent actually sent: this is the one place
    // untrusted-shaped input is decoded, and a malformed frame must fail rather
    // than read past the buffer.
    std::string fields[4];
    std::size_t pos = 0;
    for (auto &field : fields) {
        const auto *begin = payload.data() + pos;
        const void *nul = std::memchr(begin, '\0', payload.size() - pos);
        if (!nul) {
            *error = "malformed hello";
            return false;
        }
        const auto len = static_cast<std::size_t>(static_cast<const unsigned char *>(nul) - begin);
        field.assign(reinterpret_cast<const char *>(begin), len);
        pos += len + 1;
    }
    if (payload.size() - pos < kHelloFixedTail) {
        *error = "malformed hello";
        return false;
    }
    const bool anonymous = payload[pos] != 0;
    const std::uint32_t timeoutMs = getU32(payload.data() + pos + 1);

    const std::string &host = fields[0];
    if (host.empty()) {
        *error = "empty host";
        return false;
    }
    if (anonymous) {
        g_user.clear();
        g_password.clear();
    } else {
        g_user = fields[1];
        g_password = fields[2];
    }
    g_workgroup = fields[3];

    state.ctx = smbc_new_context();
    if (!state.ctx) {
        *error = "cannot allocate context";
        return false;
    }
    smbc_setFunctionAuthDataWithContext(state.ctx, authCallback);
    smbc_setOptionUseKerberos(state.ctx, 0);
    smbc_setOptionFallbackAfterKerberos(state.ctx, 1);
    smbc_setOptionNoAutoAnonymousLogin(state.ctx, 0);
    smbc_setDebug(state.ctx, 0);
    smbc_setTimeout(state.ctx, static_cast<int>(timeoutMs));
    if (!smbc_init_context(state.ctx)) {
        *error = "cannot initialise context";
        smbc_free_context(state.ctx, 1);
        state.ctx = nullptr;
        return false;
    }

    // Probe the share root so a bad credential fails here, at Hello, rather
    // than on the first Open -- the parent can then give up on this helper
    // immediately instead of after a series of confusing per-file failures.
    const std::string rootUrl = "smb://" + host + "/";
    SMBCFILE *dir = smbc_getFunctionOpendir(state.ctx)(state.ctx, rootUrl.c_str());
    if (!dir) {
        *error = "cannot connect";
        smbc_free_context(state.ctx, 1);
        state.ctx = nullptr;
        return false;
    }
    smbc_getFunctionClosedir(state.ctx)(state.ctx, dir);
    return true;
}

bool handleOpen(HelperState &state, const std::vector<unsigned char> &payload) {
    const std::string url(reinterpret_cast<const char *>(payload.data()), payload.size());
    if (url.empty())
        return sendError("empty url");
    SMBCFILE *f = smbc_getFunctionOpen(state.ctx)(state.ctx, url.c_str(), O_RDONLY, 0);
    if (!f)
        return sendError("open failed");
    const std::uint64_t id = state.nextId++;
    state.files[id] = f;
    unsigned char out[8];
    putU64(out, id);
    return sendReply(Status::Ok, out, sizeof out);
}

SMBCFILE *lookup(HelperState &state, std::uint64_t id) {
    auto it = state.files.find(id);
    return it == state.files.end() ? nullptr : it->second;
}

bool handleRead(HelperState &state, const std::vector<unsigned char> &payload) {
    if (payload.size() < 12)
        return sendError("bad read request");
    SMBCFILE *f = lookup(state, getU64(payload.data()));
    if (!f)
        return sendError("bad handle");
    std::uint32_t want = getU32(payload.data() + 8);
    if (want > kMaxReadChunk)
        want = kMaxReadChunk;

    // One reply frame carries whatever this read produced. A short read is not
    // an error: the parent loops until it has its budget or sees zero bytes,
    // which is also how it detects EOF.
    std::vector<unsigned char> buf(want);
    const ssize_t n =
        want == 0 ? 0
                  : smbc_getFunctionRead(state.ctx)(state.ctx, f, buf.data(),
                                                    static_cast<size_t>(want));
    if (n < 0)
        return sendError("read failed");
    return sendReply(Status::Ok, buf.data(), static_cast<std::uint32_t>(n));
}

bool handleSeek(HelperState &state, const std::vector<unsigned char> &payload) {
    if (payload.size() < 16)
        return sendError("bad seek request");
    SMBCFILE *f = lookup(state, getU64(payload.data()));
    if (!f)
        return sendError("bad handle");
    const auto offset = static_cast<off_t>(getU64(payload.data() + 8));
    if (smbc_getFunctionLseek(state.ctx)(state.ctx, f, offset, SEEK_SET) < 0)
        return sendError("seek failed");
    return sendOk();
}

bool handleSize(HelperState &state, const std::vector<unsigned char> &payload) {
    if (payload.size() < 8)
        return sendError("bad size request");
    SMBCFILE *f = lookup(state, getU64(payload.data()));
    if (!f)
        return sendError("bad handle");
    struct stat st;
    if (smbc_getFunctionFstat(state.ctx)(state.ctx, f, &st) != 0)
        return sendError("fstat failed");
    unsigned char out[8];
    putU64(out, static_cast<std::uint64_t>(st.st_size));
    return sendReply(Status::Ok, out, sizeof out);
}

bool handleClose(HelperState &state, const std::vector<unsigned char> &payload) {
    if (payload.size() < 8)
        return sendError("bad close request");
    const std::uint64_t id = getU64(payload.data());
    auto it = state.files.find(id);
    if (it == state.files.end())
        return sendError("bad handle");
    smbc_getFunctionClose(state.ctx)(state.ctx, it->second);
    state.files.erase(it);
    return sendOk();
}

} // namespace

int main() {
    // A write to a closed stdout must surface as EPIPE from write() so the loop
    // below can exit cleanly; the default SIGPIPE would kill the process before
    // it could close its SMB handles.
    ::signal(SIGPIPE, SIG_IGN);

    HelperState state;
    std::vector<unsigned char> payload;

    for (;;) {
        unsigned char header[kHeaderSize];
        if (!readExact(STDIN_FILENO, header, sizeof header))
            break; // parent closed the pipe: shut down

        const std::uint32_t len = getU32(header);
        const auto op = static_cast<Op>(header[4]);
        if (len > kMaxPayload) {
            // Desynchronised or hostile: the stream can't be trusted from here,
            // so stop rather than try to resynchronise.
            sendError("frame too large");
            break;
        }
        payload.resize(len);
        if (len > 0 && !readExact(STDIN_FILENO, payload.data(), len))
            break;

        // Nothing but Hello is legal before a context exists.
        if (!state.ctx && op != Op::Hello) {
            if (!sendError("not connected"))
                break;
            continue;
        }

        bool alive = true;
        switch (op) {
        case Op::Hello: {
            if (state.ctx) {
                alive = sendError("already connected");
                break;
            }
            std::string error;
            if (!handleHello(state, payload, &error)) {
                sendError(error.c_str());
                alive = false; // a helper that cannot connect has no purpose
                break;
            }
            alive = sendOk();
            break;
        }
        case Op::Open:
            alive = handleOpen(state, payload);
            break;
        case Op::Read:
            alive = handleRead(state, payload);
            break;
        case Op::Seek:
            alive = handleSeek(state, payload);
            break;
        case Op::Size:
            alive = handleSize(state, payload);
            break;
        case Op::Close:
            alive = handleClose(state, payload);
            break;
        default:
            alive = sendError("unknown opcode");
            break;
        }
        if (!alive)
            break;
    }

    if (state.ctx) {
        // shutdown_ctx=1 closes whatever files are still open, so an abrupt
        // parent exit doesn't leave server-side handles dangling.
        smbc_free_context(state.ctx, 1);
    }
    return 0;
}
