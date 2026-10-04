// pls_server.h -- the internal state every PlS:: command operates on.
// Analogous to PlainVulkan's pv_internal.h, but far smaller: a server has
// no device, swapchain, or asset tables to own.
//
// --- THREADING: THERE ISN'T ANY -----------------------------------------
// The whole server is one thread. PlS::Run() is a loop that, each tick,
// polls sockets, dispatches queued callbacks, calls the script's Tick(),
// and sleeps out the remainder of the tick period.
//
// This is a deliberate difference from the CLIENT, which does use a
// background network thread. The reason they differ is the shape of the
// program around them. A client's main thread is pinned to a render loop
// that must not stall on a socket, so reads have to happen elsewhere and be
// handed over. A server's main thread IS the simulation, and a simulation
// wants exactly the opposite property: every input applied at a
// well-defined point in a well-defined tick. Adding threads here would buy
// nothing (the socket work is trivial next to the simulation) and cost the
// determinism that makes a tick reproducible -- which is also what makes
// PlS::RecordTick / PlS::ReplayTick meaningful.
//
// The practical consequence for script authors: a slow Tick() delays
// packet processing, because nothing else is running to do it. The tick
// overrun warning in server.cpp exists to make that visible rather than
// leaving it to be diagnosed as mysterious lag.
#pragma once

#include "pls/pls_crypto.h"
#include "pls/pls_net.h"
#include "pls/pls_value.h"

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
typedef SOCKET pls_socket_t;
#define PLS_INVALID_SOCKET INVALID_SOCKET
#else
typedef int pls_socket_t;
#define PLS_INVALID_SOCKET (-1)
#endif

namespace pls {

struct Vec3 {
    double x = 0.0, y = 0.0, z = 0.0;
};

// --- entities -----------------------------------------------------------
//
// A PLAYER'S ENTITY ID EQUALS THEIR PLAYER ID. When a player connects the
// server allocates one id from the shared counter and creates their entity
// under it, before OnPlayerConnect fires. That is why a script can write
// `PlS::SetEntityPosition(player_id, x, y, z)` directly, with no lookup
// step and no separate "which entity belongs to this player" table -- and
// why PlS::BroadcastStateSelective can find a player's viewpoint just by
// treating their player id as an entity id.
struct ServerEntity {
    uint64_t id = 0;
    std::string name;
    Vec3 position;
    Vec3 rotation; // pitch, yaw, roll (degrees)
    Vec3 velocity;
    double health = 100.0;
    // Collision radius. Entities are spheres to the collision and raycast
    // code -- see physics.cpp for why that is the right level of fidelity
    // for an authoritative server.
    double radius = 0.5;
    std::string state = "alive";
    bool isPlayer = false;
    bool clampMoves = false;
};

struct Collision {
    uint64_t a = 0;
    uint64_t b = 0;
    double distance = 0.0;
};

// --- connections --------------------------------------------------------

struct PlayerConn {
    int id = 0;
    pls_socket_t fd = PLS_INVALID_SOCKET;
    net::RxStream rx;
    std::string addr;      // "ip:port", for logs and PlS::GetPlayerAddr
    double lastSeen = 0.0; // elapsed seconds at the last packet received
    Value lastInput;       // most recent input, for PlS::GetPlayerLastInput
    bool markedForClose = false;
    std::string closeReason;
    crypto::Session session;
    bool entitySpawned = false;
};

// A snapshot kept for PlS::RecordTick / PlS::ReplayTick.
struct RecordedTick {
    uint64_t tick = 0;
    double elapsed = 0.0;
    Value world; // serialized entity map, same shape as a state packet
};

// --- callbacks ----------------------------------------------------------
// Raw function pointers, for the same reason as the client's: a Value has
// no callable case, but a top-level `.pls` function compiles to a real C++
// function, so `PlS::OnPlayerConnect(OnConnect)` passes its address with no
// codegen support. The limit this implies -- a callback must be a TOP-LEVEL
// function, never one nested in a block (which compiles to a capturing
// std::function with no function-pointer form) -- is documented in the
// language reference.
using PlayerCallback = Value (*)(Value);
using PlayerCallback2 = Value (*)(Value, Value);
using TickCallback = Value (*)();

// Ceiling on simultaneously connected players.
//
// plscc passes the project's `server.max_players` through as the
// PLS_MAX_PLAYERS compile definition (see plscc.rs). Until this existed
// nothing in the runtime ever read it, so the configured limit was silently
// ignored and the accept loop took every connection offered. Two things go
// wrong without a cap, and the second is the serious one:
//
//   * each connection holds an RxStream buffer, so unbounded accepts are an
//     unbounded memory commitment to anonymous peers, and
//   * pollNetwork() multiplexes with select(), whose fd_set is a fixed-size
//     bitmap of FD_SETSIZE bits (1024 on Linux). FD_SET() on a descriptor at
//     or above that limit writes past the end of the fd_set -- undefined
//     behaviour, reachable by anyone who can open sockets to the port.
//
// The default is deliberately well under FD_SETSIZE. pollNetwork() also
// range-checks every descriptor before FD_SET as a second line of defence,
// so raising max_players past the fd_set limit degrades (players beyond it
// are refused) instead of corrupting memory.
#ifndef PLS_MAX_PLAYERS
#  define PLS_MAX_PLAYERS 64
#endif

// Tick rate used when a script never calls PlS::SetTickrate. Supplied by the
// build from plsproject.json's server.tickrate; the fallback here keeps this
// header usable on its own (unit tests, tooling) without a CMake define.
#ifndef PLS_DEFAULT_TICKRATE
#  define PLS_DEFAULT_TICKRATE 60
#endif

struct Server {
    // --- networking ---
    pls_socket_t listenSock = PLS_INVALID_SOCKET;
    std::map<int, PlayerConn> players;
    uint16_t port = 0;
    bool listening = false;
    // Enforced in acceptPending(). Initialized from the project's
    // server.max_players via the PLS_MAX_PLAYERS compile definition.
    size_t maxPlayers = static_cast<size_t>(PLS_MAX_PLAYERS);

    // --- world ---
    std::map<uint64_t, ServerEntity> entities;
    // One counter for players and entities alike -- see the note on
    // ServerEntity above.
    uint64_t nextId = 1;
    std::vector<Collision> collisionsThisTick;

    // --- loop ---
    // Initialized from the project's server.tickrate via the
    // PLS_DEFAULT_TICKRATE compile definition (see runtime/CMakeLists.txt).
    // PlS::SetTickrate overrides it at runtime.
    int tickrate = PLS_DEFAULT_TICKRATE;
    uint64_t tick = 0;
    uint64_t startMicros = 0;
    bool running = false;
    TickCallback tickHandler = nullptr;

    // --- callbacks ---
    PlayerCallback onPlayerConnect = nullptr;
    PlayerCallback2 onPlayerDisconnect = nullptr;
    PlayerCallback2 onPlayerInput = nullptr;

    // Inputs received this tick, awaiting dispatch. Queued rather than
    // dispatched from inside the socket read so that a handler is free to
    // kick a player or destroy an entity without mutating the container the
    // read loop is walking.
    std::deque<std::pair<int, Value>> inputQueue;
    std::vector<int> handshakeComplete;

    // --- broadcast settings ---
    std::string broadcastFormat = "json";
    double visibilityRange = 100.0;
    int broadcastInterval = 1;
    // Max displacement (world units per second) a player entity may travel
    // via SetEntityPosition. Direction-only input is still required; this
    // is the second line of defence against a script that accidentally
    // applies a client-supplied coordinate as a position.
    double maxPlayerSpeed = 12.0;

    // --- debug ---
    bool debugLogging = false;
    std::deque<RecordedTick> recorded;
    size_t maxRecorded = 256; // ring buffer; see PlS::RecordTick
    uint64_t bytesSent = 0;
    uint64_t bytesReceived = 0;
};

Server& server();

// --- shared internals ---------------------------------------------------

// Sends one frame to a player. Returns false and marks the connection for
// close if the write fails.
bool sendFrameTo(PlayerConn& p, const std::string& frame);
bool sendKindTo(PlayerConn& p, net::FrameKind kind, const std::string& payload);

// Serializes one entity into the object shape used by the state packet.
Value serializeEntity(const ServerEntity& e);

ServerEntity* findEntity(uint64_t id);

void logLine(const char* level, const std::string& msg);

// Drops a player: closes the socket, fires OnPlayerDisconnect, destroys
// their entity. Safe to call while iterating (it defers the erase).
void disconnectPlayer(int playerId, const std::string& reason);

namespace internal {

// Called by generated code when the script defines a top-level Tick().
// Lives in `internal` rather than `rt` so it is not mistaken for a PlS::
// command and does not need an entry in the command table.
void setTickHandler(TickCallback handler);

} // namespace internal
} // namespace pls
