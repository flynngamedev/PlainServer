// pls_runtime.h -- every `PlS::Command(...)` in the language reference,
// declared here as `pls::rt::Command(...)`. The code generator maps
// `PlS::Foo(args)` to `pls::rt::Foo(args)` mechanically (see codegen.rs):
// `::` is a generic scope operator at the *grammar* level, so nothing about
// parsing depends on this list, but codegen and the linker obviously need
// every name the reference promises to actually exist.
//
// Every function takes/returns `pls::Value` uniformly (even ones that are
// conceptually "void", like PlS::Print) so code generation never has to
// special-case "is this call used as an expression or a statement" --
// unused return values are simply discarded, as in any dynamically typed
// language. The exception is the four callback registrars, which take
// function pointers; see pls_server.h for why.
//
// See runtime/README.md for which of these are fully implemented, which are
// implemented at a deliberately simplified level, and which are stubs that
// log a warning. Nothing declared here is silently missing -- everything
// has a body.
#pragma once

#include "pls/pls_server.h"
#include "pls/pls_value.h"

namespace pls {
namespace rt {

// ---- Server setup & lifecycle ------------------------------------------
Value Listen(const Value& port);
Value SetTickrate(const Value& hz);
Value Run();
Value Shutdown();
Value GetTick();
Value GetElapsedTime();

// ---- Player & connection management ------------------------------------
Value OnPlayerConnect(PlayerCallback callback);
Value OnPlayerDisconnect(PlayerCallback2 callback);
Value OnPlayerInput(PlayerCallback2 callback);
Value GetConnectedPlayers();
Value KickPlayer(const Value& player_id, const Value& reason);
Value IsPlayerConnected(const Value& player_id);
Value GetPlayerAddr(const Value& player_id);
Value GetPlayerLastSeen(const Value& player_id);
Value CheckTimeouts(const Value& timeout_secs);

// ---- Receiving input ----------------------------------------------------
Value ReceiveInput();
Value ReceiveInputNonBlocking();
Value GetPlayerLastInput(const Value& player_id);
Value ParseInput(const Value& data, const Value& type);
Value ValidateInput(const Value& input);
Value SetMaxPlayerSpeed(const Value& units_per_sec);
Value GetPlayerSessionToken(const Value& player_id);

// ---- World state management ---------------------------------------------
Value CreateEntity(const Value& name);
Value DestroyEntity(const Value& entity_id);
Value GetEntity(const Value& entity_id);
Value GetAllEntities();
Value SetEntityPosition(const Value& entity_id, const Value& x, const Value& y, const Value& z);
Value GetEntityPosition(const Value& entity_id);
Value SetEntityRotation(const Value& entity_id, const Value& pitch, const Value& yaw,
                        const Value& roll);
Value SetEntityVelocity(const Value& entity_id, const Value& vx, const Value& vy, const Value& vz);
Value SetEntityHealth(const Value& entity_id, const Value& health);
Value GetEntityHealth(const Value& entity_id);
Value SetEntityState(const Value& entity_id, const Value& state);
Value SetEntityRadius(const Value& entity_id, const Value& radius);

// ---- Physics & game logic ------------------------------------------------
Value UpdatePhysics(const Value& delta_time);
Value CheckCollisions();
Value GetCollisionsForEntity(const Value& entity_id);
Value ApplyForce(const Value& entity_id, const Value& x, const Value& y, const Value& z);
Value RaycastFromTo(const Value& x1, const Value& y1, const Value& z1, const Value& x2,
                    const Value& y2, const Value& z2);
Value Distance(const Value& entity_id1, const Value& entity_id2);
Value FindEntitiesInSphere(const Value& cx, const Value& cy, const Value& cz, const Value& radius);

// ---- Broadcasting state --------------------------------------------------
Value BroadcastState();
Value BroadcastStateSelective();
Value SendToPlayer(const Value& player_id, const Value& data);
Value SendToAllPlayers(const Value& data);
Value SetBroadcastFormat(const Value& format);
Value SetVisibilityRange(const Value& distance);
Value SetBroadcastInterval(const Value& ticks);

// ---- Serialization helpers -----------------------------------------------
Value SerializeEntity(const Value& entity_id);
Value SerializeAllEntities();
Value DeserializeInput(const Value& data);
Value GetEntityData(const Value& entity_id);

// ---- Debugging & monitoring ----------------------------------------------
Value Print(const Value& message);
Value PrintStats();
Value EnableDebugLogging(const Value& enabled);
Value GetServerStats();
Value RecordTick(const Value& tick_number);
Value ReplayTick(const Value& tick_number);

} // namespace rt
} // namespace pls
