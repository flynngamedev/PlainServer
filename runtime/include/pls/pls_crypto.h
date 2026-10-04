// pls_crypto.h -- X25519 handshake + XChaCha20-Poly1305 sessions (Monocypher).
// Byte-identical to PlainVulkan's pv_crypto.h. Change both together.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace pls {
namespace crypto {

constexpr uint8_t kMagic[4] = {'P', 'L', 'S', 'K'};
constexpr uint8_t kVersion = 1;
constexpr int64_t kMaxTimestampSkewUs = 30LL * 1000LL * 1000LL;

struct Session {
    uint8_t secret[32]{};
    uint8_t publicKey[32]{};
    uint8_t peerPublic[32]{};
    uint8_t key[32]{};
    uint8_t token[16]{};
    uint64_t sendCounter = 1;
    uint64_t lastRecvCounter = 0;
    bool isServer = false;
    bool ready = false;
    bool havePeer = false;
};

void randomBytes(uint8_t* out, size_t n);
void generateKeyPair(Session& s);
std::string handshakePayload(const Session& s);
bool parseHandshake(const std::string& payload, Session& s);
void deriveKeys(Session& s);
void wipe(Session& s);

bool seal(Session& s, uint8_t innerKind, const std::string& inner, std::string& outAead);
bool open(Session& s, const std::string& aead, uint8_t& innerKind, std::string& inner);

std::string tokenHex(const Session& s);
uint64_t unixMicros();

} // namespace crypto
} // namespace pls
