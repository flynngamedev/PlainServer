// crypto.cpp -- Monocypher-backed session (X25519 + XChaCha20-Poly1305).
#include "pls/pls_crypto.h"

#include "monocypher.h"

#include <chrono>
#include <cstring>
#include <random>

namespace pls {
namespace crypto {

void randomBytes(uint8_t* out, size_t n) {
    std::random_device rd;
    size_t i = 0;
    while (i + 4 <= n) {
        uint32_t w = rd();
        memcpy(out + i, &w, 4);
        i += 4;
    }
    while (i < n) {
        out[i++] = static_cast<uint8_t>(rd() & 0xFF);
    }
}

uint64_t unixMicros() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(system_clock::now().time_since_epoch()).count());
}

static void appendU64BE(std::string& out, uint64_t v) {
    for (int i = 7; i >= 0; --i) out.push_back(static_cast<char>((v >> (i * 8)) & 0xFF));
}

static uint64_t readU64BE(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

void generateKeyPair(Session& s) {
    randomBytes(s.secret, 32);
    crypto_x25519_public_key(s.publicKey, s.secret);
}

std::string handshakePayload(const Session& s) {
    std::string out;
    out.append(reinterpret_cast<const char*>(kMagic), 4);
    out.push_back(static_cast<char>(kVersion));
    out.append(reinterpret_cast<const char*>(s.publicKey), 32);
    out.append(reinterpret_cast<const char*>(s.token), 16);
    return out;
}

bool parseHandshake(const std::string& payload, Session& s) {
    if (payload.size() != 53) return false;
    const auto* p = reinterpret_cast<const uint8_t*>(payload.data());
    if (p[0] != kMagic[0] || p[1] != kMagic[1] || p[2] != kMagic[2] || p[3] != kMagic[3]) return false;
    if (p[4] != kVersion) return false;
    memcpy(s.peerPublic, p + 5, 32);
    memcpy(s.token, p + 37, 16);
    s.havePeer = true;
    return true;
}

void deriveKeys(Session& s) {
    uint8_t shared[32];
    crypto_x25519(shared, s.secret, s.peerPublic);

    const uint8_t* clientPk = s.isServer ? s.peerPublic : s.publicKey;
    const uint8_t* serverPk = s.isServer ? s.publicKey : s.peerPublic;

    uint8_t msg[8 + 32 + 32 + 32 + 16];
    memcpy(msg, "pls-aead", 8);
    memcpy(msg + 8, shared, 32);
    memcpy(msg + 40, clientPk, 32);
    memcpy(msg + 72, serverPk, 32);
    memcpy(msg + 104, s.token, 16);
    crypto_blake2b(s.key, 32, msg, sizeof(msg));
    crypto_wipe(shared, sizeof(shared));
    crypto_wipe(msg, sizeof(msg));
    s.ready = true;
    s.sendCounter = 1;
    s.lastRecvCounter = 0;
}

void wipe(Session& s) {
    crypto_wipe(s.secret, sizeof(s.secret));
    crypto_wipe(s.key, sizeof(s.key));
    crypto_wipe(s.token, sizeof(s.token));
    s.ready = false;
    s.havePeer = false;
}

bool seal(Session& s, uint8_t innerKind, const std::string& inner, std::string& outAead) {
    if (!s.ready) return false;
    uint8_t nonce[24]{};
    nonce[0] = s.isServer ? 1 : 0;
    uint64_t ctr = s.sendCounter++;
    for (int i = 0; i < 8; ++i) nonce[16 + i] = static_cast<uint8_t>((ctr >> ((7 - i) * 8)) & 0xFF);

    std::string plain;
    appendU64BE(plain, unixMicros());
    plain.push_back(static_cast<char>(innerKind));
    plain += inner;

    std::string cipher(plain.size(), '\0');
    uint8_t mac[16];
    crypto_aead_lock(reinterpret_cast<uint8_t*>(cipher.data()), mac, s.key, nonce, s.token, 16,
                     reinterpret_cast<const uint8_t*>(plain.data()), plain.size());

    outAead.assign(reinterpret_cast<const char*>(nonce), 24);
    outAead.append(reinterpret_cast<const char*>(mac), 16);
    outAead += cipher;
    crypto_wipe(nonce, sizeof(nonce));
    return true;
}

bool open(Session& s, const std::string& aead, uint8_t& innerKind, std::string& inner) {
    if (!s.ready || aead.size() < 24 + 16) return false;
    const auto* p = reinterpret_cast<const uint8_t*>(aead.data());
    uint8_t nonce[24];
    memcpy(nonce, p, 24);
    uint8_t expectedDir = s.isServer ? 0 : 1;
    if (nonce[0] != expectedDir) return false;
    uint64_t ctr = readU64BE(nonce + 16);
    if (ctr <= s.lastRecvCounter) return false; // replay

    const uint8_t* mac = p + 24;
    const uint8_t* cipher = p + 40;
    size_t clen = aead.size() - 40;
    std::string plain(clen, '\0');
    if (crypto_aead_unlock(reinterpret_cast<uint8_t*>(plain.data()), mac, s.key, nonce, s.token, 16, cipher,
                           clen) != 0) {
        return false;
    }
    if (plain.size() < 9) return false;
    uint64_t ts = readU64BE(reinterpret_cast<const uint8_t*>(plain.data()));
    int64_t skew = static_cast<int64_t>(unixMicros()) - static_cast<int64_t>(ts);
    if (skew < 0) skew = -skew;
    if (skew > kMaxTimestampSkewUs) return false;

    s.lastRecvCounter = ctr;
    innerKind = static_cast<uint8_t>(plain[8]);
    inner.assign(plain, 9, std::string::npos);
    return true;
}

std::string tokenHex(const Session& s) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.resize(32);
    for (int i = 0; i < 16; ++i) {
        out[i * 2] = hex[(s.token[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[s.token[i] & 0xF];
    }
    return out;
}

} // namespace crypto
} // namespace pls
