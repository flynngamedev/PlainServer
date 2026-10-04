// server.cpp -- server lifecycle, the accept/read loop, the tick loop, and
// the input commands.
//
// The whole server runs on one thread; see the long note in pls_server.h
// for why that is a deliberate choice rather than a simplification.
#include "pls/pls_runtime.h"
#include "pls/pls_server.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define pls_close_socket closesocket
#define pls_sock_errno WSAGetLastError()
#define PLS_WOULDBLOCK WSAEWOULDBLOCK
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#define pls_close_socket ::close
#define pls_sock_errno errno
#define PLS_WOULDBLOCK EWOULDBLOCK
#endif

namespace pls {

Server& server() {
    static Server s;
    return s;
}

namespace {

// Winsock needs an explicit per-process startup and refcounts its cleanup,
// so a function-local static gets the pairing exactly right: initialized
// once, on first use, torn down at exit.
struct SocketSubsystem {
    SocketSubsystem() {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    }
    ~SocketSubsystem() {
#ifdef _WIN32
        WSACleanup();
#endif
    }
};

void ensureSocketSubsystem() {
    static SocketSubsystem instance;
    (void)instance;
}

void setNonBlocking(pls_socket_t s) {
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags >= 0) fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
}

void setNoDelay(pls_socket_t s) {
    // Nagle's algorithm holds small writes back waiting for an ACK, which
    // is precisely wrong for a 60Hz stream of small state packets.
    int yes = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof(yes));
}

double elapsedSeconds() {
    Server& s = server();
    if (s.startMicros == 0) return 0.0;
    return static_cast<double>(net::nowMicros() - s.startMicros) / 1e6;
}

bool wouldBlock(int err) {
#ifdef _WIN32
    return err == PLS_WOULDBLOCK;
#else
    return err == EWOULDBLOCK || err == EAGAIN || err == EINTR;
#endif
}

// Closes sockets, fires OnPlayerDisconnect, and destroys player entities
// for every connection marked during the read loop. Deferred like this so a
// handler is free to mutate `players` without invalidating an iterator the
// caller is still holding.
void reapClosed() {
    Server& s = server();
    std::vector<int> dead;
    for (auto& kv : s.players) {
        if (kv.second.markedForClose) dead.push_back(kv.first);
    }
    for (int id : dead) {
        auto it = s.players.find(id);
        if (it == s.players.end()) continue;
        std::string reason = it->second.closeReason;
        if (it->second.fd != PLS_INVALID_SOCKET) pls_close_socket(it->second.fd);
        s.players.erase(it);

        // The entity is destroyed BEFORE the callback so that a handler
        // which broadcasts state doesn't include an entity belonging to a
        // player who is already gone.
        s.entities.erase(static_cast<uint64_t>(id));

        if (s.onPlayerDisconnect) {
            s.onPlayerDisconnect(Value(static_cast<int64_t>(id)), Value(reason));
        }
        logLine("INFO", "player " + std::to_string(id) + " disconnected: " + reason);
    }
}

void handleInnerFrame(PlayerConn& p, net::FrameKind kind, const std::string& payload) {
    Server& s = server();
    p.lastSeen = elapsedSeconds();

    switch (kind) {
        case net::FRAME_PING:
            sendKindTo(p, net::FRAME_PONG, payload);
            return;
        case net::FRAME_PONG:
            return;
        case net::FRAME_JSON: {
            std::string err;
            json::Json j = json::Json::parse(payload, &err);
            if (!err.empty()) {
                logLine("WARN", "player " + std::to_string(p.id) +
                                    " sent malformed JSON, ignoring: " + err);
                return;
            }
            Value input = net::jsonToValue(j);
            p.lastInput = input;
            s.inputQueue.emplace_back(p.id, input);
            return;
        }
        case net::FRAME_RAW: {
            Value input(payload);
            p.lastInput = input;
            s.inputQueue.emplace_back(p.id, input);
            return;
        }
        default:
            break;
    }
    if (s.debugLogging) {
        logLine("DEBUG", "unknown inner frame kind " + std::to_string(static_cast<int>(kind)));
    }
}

void spawnPlayerEntity(PlayerConn& p) {
    if (p.entitySpawned) return;
    Server& s = server();
    ServerEntity e;
    e.id = static_cast<uint64_t>(p.id);
    e.name = "Player_" + std::to_string(p.id);
    e.isPlayer = true;
    s.entities[e.id] = e;
    p.entitySpawned = true;
    s.handshakeComplete.push_back(p.id);
}

void handleFrame(PlayerConn& p, net::FrameKind kind, const std::string& payload) {
    Server& s = server();

    if (kind == net::FRAME_HANDSHAKE) {
        if (p.session.ready) return;
        if (!crypto::parseHandshake(payload, p.session)) {
            p.markedForClose = true;
            p.closeReason = "malformed handshake";
            return;
        }
        crypto::randomBytes(p.session.token, 16);
        crypto::deriveKeys(p.session);
        if (!sendFrameTo(p, net::buildFrame(net::FRAME_HANDSHAKE, crypto::handshakePayload(p.session)))) {
            return;
        }
        spawnPlayerEntity(p);
        logLine("INFO", "player " + std::to_string(p.id) + " session established token=" +
                            crypto::tokenHex(p.session));
        return;
    }

    if (!p.session.ready) {
        p.markedForClose = true;
        p.closeReason = "expected handshake before traffic";
        return;
    }

    if (kind == net::FRAME_AEAD) {
        uint8_t innerKind = 0;
        std::string inner;
        if (!crypto::open(p.session, payload, innerKind, inner)) {
            p.markedForClose = true;
            p.closeReason = "replay, skew, or MAC failure";
            return;
        }
        handleInnerFrame(p, static_cast<net::FrameKind>(innerKind), inner);
        return;
    }

    p.markedForClose = true;
    p.closeReason = "plaintext frame after handshake";
}

void acceptPending(std::vector<int>& newlyConnected) {
    (void)newlyConnected;
    Server& s = server();
    for (;;) {
        sockaddr_in addr{};
#ifdef _WIN32
        int addrLen = sizeof(addr);
#else
        socklen_t addrLen = sizeof(addr);
#endif
        pls_socket_t c = ::accept(s.listenSock, reinterpret_cast<sockaddr*>(&addr), &addrLen);
        if (c == PLS_INVALID_SOCKET) break;

        // Refuse the connection rather than growing past the configured
        // ceiling. Without this the project's server.max_players was silently
        // ignored: every offered connection was accepted, which is both an
        // unbounded memory commitment to anonymous peers and -- once the
        // process passes FD_SETSIZE descriptors -- undefined behaviour in
        // pollNetwork()'s FD_SET. Closing immediately is the honest signal;
        // there is no room to hold the socket open for later.
        if (s.players.size() >= s.maxPlayers) {
            pls_close_socket(c);
            if (s.debugLogging) {
                logLine("DEBUG", "refused a connection: server is full (" +
                                     std::to_string(s.players.size()) + "/" +
                                     std::to_string(s.maxPlayers) + " players)");
            }
            break;
        }

#ifndef _WIN32
        // On POSIX an fd_set is a fixed bitmap indexed by descriptor number,
        // so a descriptor at or above FD_SETSIZE cannot be watched -- FD_SET
        // would write out of bounds. (Windows fd_set is an array of handles
        // with an FD_SETSIZE *count* limit instead, which the maxPlayers cap
        // above already covers.) This is unreachable at the default limit and
        // exists so raising max_players degrades safely instead of corrupting
        // memory.
        if (c >= static_cast<pls_socket_t>(FD_SETSIZE)) {
            pls_close_socket(c);
            logLine("WARN", "refused a connection: descriptor " + std::to_string(c) +
                                " is beyond FD_SETSIZE (" + std::to_string(FD_SETSIZE) +
                                "), which select() cannot watch");
            break;
        }
#endif

        setNonBlocking(c);
        setNoDelay(c);

        int id = static_cast<int>(s.nextId++);
        PlayerConn p;
        p.id = id;
        p.fd = c;
        p.lastSeen = elapsedSeconds();
        p.session.isServer = true;
        crypto::generateKeyPair(p.session);
        char ipbuf[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &addr.sin_addr, ipbuf, sizeof(ipbuf));
        p.addr = std::string(ipbuf) + ":" + std::to_string(ntohs(addr.sin_port));
        s.players[id] = std::move(p);

        // Entity + OnPlayerConnect wait for the X25519 handshake so a
        // connection that never speaks PLSK cannot occupy a player slot
        // in the simulation.
        logLine("INFO", "socket from " + s.players[id].addr + " as player " + std::to_string(id) +
                            " (awaiting handshake)");
    }
}

// Services sockets for up to `timeoutMicros`. Returns immediately after one
// select() -- the caller loops.
void pollNetwork(int64_t timeoutMicros) {
    Server& s = server();
    if (timeoutMicros < 0) timeoutMicros = 0;

    fd_set readSet;
    FD_ZERO(&readSet);
    pls_socket_t maxFd = 0;

    // FD_SET on a descriptor at or above FD_SETSIZE writes past the end of
    // the fd_set on POSIX. acceptPending() already refuses such descriptors,
    // so this is defence in depth -- it keeps the invariant local to the one
    // function that actually depends on it.
    auto watch = [&](pls_socket_t fd) {
#ifndef _WIN32
        if (fd < 0 || fd >= static_cast<pls_socket_t>(FD_SETSIZE)) return false;
#endif
        FD_SET(fd, &readSet);
        if (fd > maxFd) maxFd = fd;
        return true;
    };

    if (s.listenSock != PLS_INVALID_SOCKET) {
        watch(s.listenSock);
    }
    for (auto& kv : s.players) {
        if (kv.second.markedForClose) continue;
        watch(kv.second.fd);
    }

    timeval tv;
    tv.tv_sec = static_cast<long>(timeoutMicros / 1000000);
    tv.tv_usec = static_cast<long>(timeoutMicros % 1000000);
    int ready = ::select(static_cast<int>(maxFd) + 1, &readSet, nullptr, nullptr, &tv);
    if (ready <= 0) return;

    std::vector<int> newlyConnected;
    if (s.listenSock != PLS_INVALID_SOCKET && FD_ISSET(s.listenSock, &readSet)) {
        acceptPending(newlyConnected);
    }

    std::string payload;
    for (auto& kv : s.players) {
        PlayerConn& p = kv.second;
        if (p.markedForClose || !FD_ISSET(p.fd, &readSet)) continue;

        char buf[8192];
        int n = static_cast<int>(::recv(p.fd, buf, sizeof(buf), 0));
        if (n > 0) {
            s.bytesReceived += static_cast<uint64_t>(n);
            p.rx.buf.append(buf, static_cast<size_t>(n));
            net::FrameKind kind;
            bool oversize = false;
            while (p.rx.next(kind, payload, oversize)) {
                handleFrame(p, kind, payload);
            }
            if (oversize) {
                p.markedForClose = true;
                p.closeReason = "protocol error: frame length out of range";
            }
        } else if (n == 0) {
            p.markedForClose = true;
            p.closeReason = "closed by remote host";
        } else {
            int err = pls_sock_errno;
            if (!wouldBlock(err)) {
                p.markedForClose = true;
                // A client that closes its socket while state packets are
                // still queued for it triggers an RST rather than a clean
                // FIN, which surfaces here as ECONNRESET. That is an
                // ordinary disconnect -- naming it "recv failed" would send
                // someone hunting a network fault that isn't there.
#ifdef _WIN32
                p.closeReason = (err == WSAECONNRESET) ? "connection reset by peer" : "recv failed";
#else
                p.closeReason = (err == ECONNRESET) ? "connection reset by peer" : "recv failed";
#endif
            }
        }
    }

    reapClosed();

    for (int id : s.handshakeComplete) newlyConnected.push_back(id);
    s.handshakeComplete.clear();

    // Connect handlers run last, after reaping, so the world a handler sees
    // is settled: no half-closed connections, no entities belonging to
    // players who left in the same poll.
    for (int id : newlyConnected) {
        if (s.onPlayerConnect) s.onPlayerConnect(Value(static_cast<int64_t>(id)));
        if (ServerEntity* e = findEntity(static_cast<uint64_t>(id))) e->clampMoves = true;
    }
}

// Hands every queued input to the script's OnPlayerInput handler. Drained
// into a local first so a handler is free to queue more (or disconnect the
// sender) without this loop tripping over the change.
void dispatchInputs() {
    Server& s = server();
    if (!s.onPlayerInput) {
        // With no handler registered the queue would grow without bound;
        // the script is presumably polling with PlS::ReceiveInput instead,
        // so leave the queue for it -- but cap it, so a script that
        // registers neither doesn't leak memory for the life of the match.
        const size_t kMaxUnread = 4096;
        while (s.inputQueue.size() > kMaxUnread) s.inputQueue.pop_front();
        return;
    }
    std::deque<std::pair<int, Value>> batch;
    batch.swap(s.inputQueue);
    for (auto& entry : batch) {
        s.onPlayerInput(Value(static_cast<int64_t>(entry.first)), entry.second);
    }
}

} // namespace

// --- shared internals ----------------------------------------------------

void logLine(const char* level, const std::string& msg) {
    std::printf("[%8.2f] [%s] %s\n", elapsedSeconds(), level, msg.c_str());
    std::fflush(stdout);
}

ServerEntity* findEntity(uint64_t id) {
    Server& s = server();
    auto it = s.entities.find(id);
    return it == s.entities.end() ? nullptr : &it->second;
}

bool sendKindTo(PlayerConn& p, net::FrameKind kind, const std::string& payload) {
    if (kind == net::FRAME_HANDSHAKE) {
        return sendFrameTo(p, net::buildFrame(kind, payload));
    }
    if (!p.session.ready) return false;
    std::string aead;
    if (!crypto::seal(p.session, static_cast<uint8_t>(kind), payload, aead)) return false;
    return sendFrameTo(p, net::buildFrame(net::FRAME_AEAD, aead));
}

bool sendFrameTo(PlayerConn& p, const std::string& frame) {
    if (p.fd == PLS_INVALID_SOCKET || p.markedForClose) return false;
    Server& s = server();
    size_t off = 0;
    int spins = 0;
    while (off < frame.size()) {
        int n = static_cast<int>(
            ::send(p.fd, frame.data() + off, static_cast<int>(frame.size() - off), 0));
        if (n > 0) {
            off += static_cast<size_t>(n);
            spins = 0;
            continue;
        }
        if (n < 0 && wouldBlock(pls_sock_errno)) {
            // This player's socket buffer is full -- they are not reading
            // fast enough. Sleeping the whole server for one slow client
            // would punish everyone, so give up quickly and drop them
            // rather than letting one connection dictate the tick rate.
            if (++spins > 50) {
                p.markedForClose = true;
                p.closeReason = "send buffer full (client not reading)";
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        p.markedForClose = true;
        p.closeReason = "send failed";
        return false;
    }
    s.bytesSent += frame.size();
    return true;
}

void disconnectPlayer(int playerId, const std::string& reason) {
    Server& s = server();
    auto it = s.players.find(playerId);
    if (it == s.players.end()) return;
    it->second.markedForClose = true;
    it->second.closeReason = reason;
}

namespace internal {

void setTickHandler(TickCallback handler) {
    server().tickHandler = handler;
}

} // namespace internal

namespace rt {

// ---- Server setup & lifecycle -------------------------------------------

Value Listen(const Value& port) {
    ensureSocketSubsystem();
    Server& s = server();

    pls_socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == PLS_INVALID_SOCKET) {
        logLine("ERROR", "PlS::Listen: could not create socket");
        return Value(false);
    }
    // Without SO_REUSEADDR, restarting the server during development fails
    // to bind for the duration of TIME_WAIT (up to two minutes).
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));

    uint16_t p = static_cast<uint16_t>(port.asInt());
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(p);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        pls_close_socket(fd);
        logLine("ERROR", "PlS::Listen: port " + std::to_string(p) + " is already in use");
        return Value(false);
    }
    if (::listen(fd, 64) != 0) {
        pls_close_socket(fd);
        logLine("ERROR", "PlS::Listen: listen() failed");
        return Value(false);
    }
    setNonBlocking(fd);

    if (s.listenSock != PLS_INVALID_SOCKET) pls_close_socket(s.listenSock);
    s.listenSock = fd;
    s.port = p;
    s.listening = true;
    if (s.startMicros == 0) s.startMicros = net::nowMicros();
    logLine("INFO", "listening on port " + std::to_string(p));
    return Value(true);
}

Value SetTickrate(const Value& hz) {
    Server& s = server();
    int rate = static_cast<int>(hz.asInt());
    if (rate <= 0) {
        logLine("WARN", "PlS::SetTickrate: ignoring non-positive tickrate " +
                            std::to_string(rate) + "; keeping " + std::to_string(s.tickrate));
        return Value(false);
    }
    if (rate > 1000) {
        // Above ~1kHz the sleep granularity of a general-purpose OS makes
        // the requested rate unachievable, so honouring it would just mean
        // busy-spinning a core to miss the target anyway.
        logLine("WARN", "PlS::SetTickrate: " + std::to_string(rate) +
                            "Hz exceeds the 1000Hz ceiling; clamping");
        rate = 1000;
    }
    s.tickrate = rate;
    return Value(true);
}

Value Run() {
    Server& s = server();
    if (!s.listening) {
        logLine("ERROR", "PlS::Run: not listening -- call PlS::Listen(port) first");
        return Value(false);
    }
    s.running = true;
    if (s.startMicros == 0) s.startMicros = net::nowMicros();

    const int64_t period = 1000000 / s.tickrate;
    int64_t nextTick = static_cast<int64_t>(net::nowMicros()) + period;
    logLine("INFO", "running at " + std::to_string(s.tickrate) + "Hz");

    while (s.running) {
        int64_t now = static_cast<int64_t>(net::nowMicros());
        if (now < nextTick) {
            // Service the network in the gap between ticks. select() with
            // exactly the remaining budget means the server is both fully
            // responsive to packets and completely idle (0% CPU) when
            // nothing is arriving.
            pollNetwork(nextTick - now);
            continue;
        }

        dispatchInputs();
        if (s.tickHandler) s.tickHandler();
        reapClosed(); // a handler may have kicked someone
        s.tick++;

        nextTick += period;
        int64_t after = static_cast<int64_t>(net::nowMicros());
        if (after > nextTick) {
            // The tick took longer than its budget. Rather than trying to
            // catch up -- which compounds, since the backlog makes each
            // subsequent tick later still -- resynchronize and say so. A
            // silent overrun is otherwise diagnosed by players as lag with
            // no server-side evidence at all.
            if (s.debugLogging) {
                logLine("WARN", "tick " + std::to_string(s.tick) + " overran its budget by " +
                                    std::to_string((after - nextTick) / 1000) + "ms");
            }
            nextTick = after + period;
        }
    }

    logLine("INFO", "server loop exited after " + std::to_string(s.tick) + " ticks");
    return Value(true);
}

Value Shutdown() {
    Server& s = server();
    s.running = false;
    for (auto& kv : s.players) {
        if (kv.second.fd != PLS_INVALID_SOCKET) pls_close_socket(kv.second.fd);
    }
    s.players.clear();
    if (s.listenSock != PLS_INVALID_SOCKET) {
        pls_close_socket(s.listenSock);
        s.listenSock = PLS_INVALID_SOCKET;
    }
    s.listening = false;
    logLine("INFO", "shutdown complete");
    return Value(true);
}

Value GetTick() {
    return Value(static_cast<int64_t>(server().tick));
}

Value GetElapsedTime() {
    return Value(elapsedSeconds());
}

// ---- Player & connection management --------------------------------------

Value OnPlayerConnect(PlayerCallback callback) {
    server().onPlayerConnect = callback;
    return Value(true);
}

Value OnPlayerDisconnect(PlayerCallback2 callback) {
    server().onPlayerDisconnect = callback;
    return Value(true);
}

Value OnPlayerInput(PlayerCallback2 callback) {
    server().onPlayerInput = callback;
    return Value(true);
}

Value GetConnectedPlayers() {
    Value out = Value::MakeArray();
    for (auto& kv : server().players) {
        if (kv.second.markedForClose) continue;
        out.arrayRef().push_back(Value(static_cast<int64_t>(kv.first)));
    }
    return out;
}

Value KickPlayer(const Value& player_id, const Value& reason) {
    int id = static_cast<int>(player_id.asInt());
    std::string why = reason.asString();
    if (why.empty()) why = "kicked";
    Server& s = server();
    if (s.players.find(id) == s.players.end()) return Value(false);
    disconnectPlayer(id, why);
    return Value(true);
}

Value IsPlayerConnected(const Value& player_id) {
    Server& s = server();
    auto it = s.players.find(static_cast<int>(player_id.asInt()));
    return Value(it != s.players.end() && !it->second.markedForClose);
}

Value GetPlayerAddr(const Value& player_id) {
    Server& s = server();
    auto it = s.players.find(static_cast<int>(player_id.asInt()));
    return it == s.players.end() ? Value(std::string()) : Value(it->second.addr);
}

Value GetPlayerLastSeen(const Value& player_id) {
    Server& s = server();
    auto it = s.players.find(static_cast<int>(player_id.asInt()));
    if (it == s.players.end()) return Value(0.0);
    // Seconds SINCE the last packet, which is what a timeout check wants.
    return Value(elapsedSeconds() - it->second.lastSeen);
}

Value CheckTimeouts(const Value& timeout_secs) {
    // Not in the original command list, but the reference's own example
    // server calls PlS::CheckTimeouts -- and every server needs it, since a
    // client that loses power never closes its socket and would otherwise
    // occupy a slot until the OS keepalive expires (hours, by default).
    double limit = timeout_secs.asFloat();
    if (limit <= 0.0) return Value(static_cast<int64_t>(0));
    Server& s = server();
    double now = elapsedSeconds();
    int64_t kicked = 0;
    for (auto& kv : s.players) {
        if (kv.second.markedForClose) continue;
        if (now - kv.second.lastSeen > limit) {
            disconnectPlayer(kv.first, "timeout");
            ++kicked;
        }
    }
    return Value(kicked);
}

// ---- Receiving input ------------------------------------------------------

Value ReceiveInput() {
    // "Blocking" on a single-threaded server means *pumping the network
    // until something arrives* -- if this simply slept, no input could ever
    // arrive, since this thread is the only thing that reads sockets.
    Server& s = server();
    while (s.inputQueue.empty()) {
        if (!s.listening) return Value();
        pollNetwork(50 * 1000);
    }
    auto entry = s.inputQueue.front();
    s.inputQueue.pop_front();
    Value out = Value::MakeObject();
    out.objectRef()["player_id"] = Value(static_cast<int64_t>(entry.first));
    out.objectRef()["input_data"] = entry.second;
    return out;
}

Value ReceiveInputNonBlocking() {
    Server& s = server();
    if (s.inputQueue.empty()) {
        pollNetwork(0); // one non-blocking sweep, so a caller polling in a
                        // tight loop still makes progress
    }
    if (s.inputQueue.empty()) return Value();
    auto entry = s.inputQueue.front();
    s.inputQueue.pop_front();
    Value out = Value::MakeObject();
    out.objectRef()["player_id"] = Value(static_cast<int64_t>(entry.first));
    out.objectRef()["input_data"] = entry.second;
    return out;
}

Value GetPlayerLastInput(const Value& player_id) {
    Server& s = server();
    auto it = s.players.find(static_cast<int>(player_id.asInt()));
    return it == s.players.end() ? Value() : it->second.lastInput;
}

Value ParseInput(const Value& data, const Value& type) {
    std::string kind = type.asString();
    if (kind.empty() || kind == "json") {
        std::string err;
        json::Json j = json::Json::parse(data.asString(), &err);
        if (!err.empty()) {
            logLine("WARN", "PlS::ParseInput: malformed JSON: " + err);
            return Value();
        }
        return net::jsonToValue(j);
    }
    // msgpack/binary are accepted by the API but not implemented; see
    // runtime/README.md. Returning the input unchanged (rather than null)
    // keeps a script that sets a format it doesn't actually depend on
    // working, while the warning makes the gap visible.
    logLine("WARN", "PlS::ParseInput: format '" + kind +
                        "' is not implemented; returning the value unparsed");
    return data;
}

Value ValidateInput(const Value& input) {
    // A cheap, universal sanity check on data that came from a client and
    // is therefore untrusted. It is NOT a substitute for game-specific
    // validation -- it cannot know that a move of 400 units in one tick is
    // impossible in your game -- but it does reject the inputs that would
    // corrupt the simulation itself rather than merely cheat at it: a
    // missing action, or a NaN/Infinity that would silently poison every
    // position it touches and cannot even be represented in JSON.
    if (!input.isObject()) return Value(false);

    Value& mut = const_cast<Value&>(input);
    for (const auto& kv : mut.objectRef()) {
        if (kv.first == "position" || kv.first == "pos" || kv.first == "transform") {
            return Value(false);
        }
    }

    Value action = member_get(input, "action");
    if (action.type() != ValueType::String || action.asString().empty()) return Value(false);
    const std::string act = action.asString();
    if (act == "set_position" || act == "teleport" || act == "position") return Value(false);

    const char* axes[] = {"x", "y", "z"};
    for (const char* axis : axes) {
        Value v = member_get(input, axis);
        if (v.isNull()) continue;
        if (!v.isNumber()) return Value(false);
        double d = v.asFloat();
        if (!std::isfinite(d)) return Value(false);
        if (act == "move" && std::fabs(d) > 1.5) return Value(false);
    }
    if (act == "move") {
        double x = member_get(input, "x").asFloat();
        double y = member_get(input, "y").asFloat();
        double z = member_get(input, "z").asFloat();
        double len = std::sqrt(x * x + y * y + z * z);
        if (len > 1.0001) {
            mut.objectRef()["x"] = Value(x / len);
            mut.objectRef()["y"] = Value(y / len);
            mut.objectRef()["z"] = Value(z / len);
        }
    }
    return Value(true);
}

Value SetMaxPlayerSpeed(const Value& units_per_sec) {
    double v = units_per_sec.asFloat();
    if (!(v > 0.0) || !std::isfinite(v)) return Value(false);
    server().maxPlayerSpeed = v;
    return Value(true);
}

Value GetPlayerSessionToken(const Value& player_id) {
    Server& s = server();
    auto it = s.players.find(static_cast<int>(player_id.asInt()));
    if (it == s.players.end() || !it->second.session.ready) return Value();
    return Value(crypto::tokenHex(it->second.session));
}

} // namespace rt
} // namespace pls
