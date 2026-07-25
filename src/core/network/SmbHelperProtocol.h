#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Wire format shared by the SMB helper subprocess (src/smbhelper) and its
// in-app client (SmbHelperClient).
//
// Deliberately free of Qt and of every other ttc header: the helper is a tiny
// standalone executable that links libsmbclient and nothing else, so that a
// crash inside libsmbclient takes down only that process. Keeping this header
// dependency-free is what lets both sides share one definition of the format
// instead of hand-writing it twice.
//
// Why a subprocess at all: libsmbclient cannot be driven from two threads at
// once, not even through separate SMBCCTX contexts -- its talloc pools and
// parsed smb.conf are process-global state. Process isolation gives each
// context its own copy of that state, which is the same conclusion GVfs
// reached (one gvfsd-smb process per mount).
namespace smbhelper {

// Every frame is: [u32 payloadLen][u8 code][payload]. The length prefix comes
// first so a reader always knows how much to pull before interpreting
// anything, which is what makes bulk file bytes safe to carry inline.
constexpr std::size_t kHeaderSize = 5;

// Hard ceiling on a single frame's payload, enforced on both sides. Bounds the
// memory a malformed or hostile length prefix can make the peer allocate.
constexpr std::uint32_t kMaxPayload = 1u << 20; // 1 MiB

// Largest READ the helper will serve in one frame. The caller streams a file by
// issuing repeated bounded reads rather than asking for the whole thing, so a
// 4 MB video prefix never becomes a 4 MB allocation on either side.
constexpr std::uint32_t kMaxReadChunk = 256u * 1024u;

// Request opcodes (client -> helper).
enum class Op : std::uint8_t {
    Hello = 1, // credentials + connect; must be the first frame
    Open = 2,  // payload: smb:// URL bytes -> reply payload: u64 handle id
    Read = 3,  // payload: u64 handle, u32 count -> reply payload: the bytes read
    Seek = 4,  // payload: u64 handle, u64 offset
    Size = 5,  // payload: u64 handle -> reply payload: u64 size
    Close = 6, // payload: u64 handle
};

// Reply status (helper -> client).
enum class Status : std::uint8_t {
    Ok = 0,
    Error = 1, // the operation failed; payload carries a short reason
};

// Little-endian fixed-width codecs. Explicit byte shuffling rather than a
// memcpy of the native representation: the format is a contract, so it must not
// silently depend on the host's endianness.
inline void putU32(unsigned char *dst, std::uint32_t v) {
    dst[0] = static_cast<unsigned char>(v & 0xFF);
    dst[1] = static_cast<unsigned char>((v >> 8) & 0xFF);
    dst[2] = static_cast<unsigned char>((v >> 16) & 0xFF);
    dst[3] = static_cast<unsigned char>((v >> 24) & 0xFF);
}

inline std::uint32_t getU32(const unsigned char *src) {
    return static_cast<std::uint32_t>(src[0]) | (static_cast<std::uint32_t>(src[1]) << 8) |
           (static_cast<std::uint32_t>(src[2]) << 16) | (static_cast<std::uint32_t>(src[3]) << 24);
}

inline void putU64(unsigned char *dst, std::uint64_t v) {
    putU32(dst, static_cast<std::uint32_t>(v & 0xFFFFFFFFu));
    putU32(dst + 4, static_cast<std::uint32_t>((v >> 32) & 0xFFFFFFFFu));
}

inline std::uint64_t getU64(const unsigned char *src) {
    return static_cast<std::uint64_t>(getU32(src)) |
           (static_cast<std::uint64_t>(getU32(src + 4)) << 32);
}

// Writes a frame header into `dst` (must have room for kHeaderSize bytes).
inline void writeHeader(unsigned char *dst, std::uint32_t payloadLen, std::uint8_t code) {
    putU32(dst, payloadLen);
    dst[4] = code;
}

// The Hello payload is a sequence of NUL-terminated UTF-8 fields followed by
// two fixed-width values:
//
//   host \0 user \0 password \0 workgroup \0 [u8 anonymous][u32 timeoutMs]
//
// Credentials travel here, inside the frame stream on a private socketpair,
// and never as argv or environment: argv is world-readable through
// /proc/<pid>/cmdline (any user on the machine can watch `ps`), and the
// environment is inherited by anything the helper might exec. A socket the two
// processes hold privately is visible to neither.
constexpr std::size_t kHelloFixedTail = 5; // u8 anonymous + u32 timeoutMs

} // namespace smbhelper
