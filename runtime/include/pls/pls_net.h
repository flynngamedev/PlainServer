// pls_net.h -- the wire format, shared verbatim with PlainVulkan's client
// (see PlainVulkan/runtime/include/pv/pv_net.h, which documents the same
// layout from the other side).
//
// Keeping the two definitions byte-identical is what makes a PlainS server
// and a PlainVulkan client interoperable, and it is the single easiest
// thing in this whole system to break silently: a mismatch produces no
// compile error, no link error, and no runtime exception -- just a
// connection that hangs or delivers garbage. If you change anything here,
// change pv_net.h in the same commit.
#pragma once

#include "pls/pls_json.h"
#include "pls/pls_value.h"

#include <cstdint>
#include <string>

namespace pls {
namespace net {

// Every message on the wire is one frame:
//
//   [0..4)   uint32, big-endian: byte length of everything that follows
//   [4]      uint8: frame kind (below)
//   [5..)    payload, `length - 1` bytes
enum FrameKind : uint8_t {
    FRAME_JSON = 0, // payload is UTF-8 JSON; surfaces to scripts as a Value
    FRAME_RAW = 1,  // payload is opaque bytes; surfaces as a Value string
    FRAME_PING = 2, // payload is uint64 big-endian microsecond stamp
    FRAME_PONG = 3, // payload is the stamp from the PING, echoed verbatim
    FRAME_HANDSHAKE = 4, // cleartext X25519 public key + session token (see pls_crypto.h)
    FRAME_AEAD = 5,      // XChaCha20-Poly1305 wrapper around an inner frame
};

constexpr uint32_t kMaxFrameBytes = 16u * 1024u * 1024u;

std::string buildFrame(FrameKind kind, const std::string& payload);

// One connection's receive-side accumulator: TCP is a stream, so a single
// recv() can deliver half a frame or three-and-a-half.
struct RxStream {
    std::string buf;

    // Returns true and fills kind/payload when a complete frame is ready.
    // `oversize` signals a length prefix beyond kMaxFrameBytes, which is
    // unrecoverable -- the stream is desynchronized and there is no in-band
    // way to find where the next frame starts, so the caller drops the
    // connection.
    bool next(FrameKind& kind, std::string& payload, bool& oversize);
};

// Handles serialize as their integer id, matching the client.
json::Json valueToJson(const Value& v);
Value jsonToValue(const json::Json& j);

uint64_t nowMicros();

} // namespace net
} // namespace pls
