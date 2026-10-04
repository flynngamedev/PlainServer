// broadcast.cpp -- sending world state to clients, plus the serialization
// helpers scripts use to build their own payloads.
//
// The state packet this produces is consumed by Pv::DeserializeState on the
// client; its exact shape is documented in PlainVulkan's network_scene.cpp.
#include "pls/pls_runtime.h"
#include "pls/pls_server.h"

#include <cmath>
#include <string>

namespace pls {
namespace rt {

namespace {

// Wraps a map of serialized entities in the state envelope.
// `static` on these file-local helpers is redundant inside an anonymous
// namespace, but it is the convention this codebase uses to keep helpers in a
// command file visibly distinct from commands -- see
// scripts/check_command_coverage.sh.
static Value stateEnvelope(Value entities) {
    Server& s = server();
    Value out = Value::MakeObject();
    auto& o = out.objectRef();
    o["type"] = Value(std::string("state"));
    o["tick"] = Value(static_cast<int64_t>(s.tick));
    o["entities"] = entities;
    return out;
}

std::string encodePacket(const Value& packet) {
    Server& s = server();
    if (s.broadcastFormat != "json") {
        // Accepted by SetBroadcastFormat but not implemented; the warning
        // was already issued there, so this silently uses JSON rather than
        // logging once per tick.
        //
        // Falling back (instead of refusing to send) is the safer failure:
        // a script that set a format it doesn't truly depend on keeps
        // working, and the client -- which only speaks JSON frames -- can
        // still read what arrives.
    }
    return net::valueToJson(packet).dump(-1); // compact: no wire padding
}

// Entities within `visibilityRange` of the given player's own entity.
// Falls back to everything if the player has no entity (which shouldn't
// happen, since one is created on connect, but a script is free to destroy
// it and should not get an empty world as a result).
static Value visibleTo(int playerId) {
    Server& s = server();
    Value entities = Value::MakeObject();
    auto& out = entities.objectRef();

    ServerEntity* viewer = findEntity(static_cast<uint64_t>(playerId));
    if (!viewer) {
        for (auto& kv : s.entities) out[std::to_string(kv.first)] = serializeEntity(kv.second);
        return entities;
    }

    double r2 = s.visibilityRange * s.visibilityRange;
    for (auto& kv : s.entities) {
        const ServerEntity& e = kv.second;
        // A player is always sent their own entity regardless of range --
        // without this, a viewer whose radius somehow excluded itself would
        // stop receiving its own authoritative position and desync.
        if (e.id != viewer->id) {
            double dx = e.position.x - viewer->position.x;
            double dy = e.position.y - viewer->position.y;
            double dz = e.position.z - viewer->position.z;
            if (dx * dx + dy * dy + dz * dz > r2) continue;
        }
        out[std::to_string(kv.first)] = serializeEntity(e);
    }
    return entities;
}

bool shouldBroadcastThisTick() {
    Server& s = server();
    if (s.broadcastInterval <= 1) return true;
    return (s.tick % static_cast<uint64_t>(s.broadcastInterval)) == 0;
}

} // namespace

Value BroadcastState() {
    Server& s = server();
    if (!shouldBroadcastThisTick()) return Value(false);

    // Serialized ONCE for everyone. With a full broadcast every player gets
    // identical bytes, so building the packet per player would multiply the
    // JSON encoding cost by the player count for no difference in output.
    Value entities = Value::MakeObject();
    for (auto& kv : s.entities) {
        entities.objectRef()[std::to_string(kv.first)] = serializeEntity(kv.second);
    }
    std::string body = encodePacket(stateEnvelope(entities));

    int64_t sent = 0;
    for (auto& kv : s.players) {
        if (kv.second.markedForClose || !kv.second.session.ready) continue;
        if (sendKindTo(kv.second, net::FRAME_JSON, body)) ++sent;
    }
    return Value(sent);
}

Value BroadcastStateSelective() {
    Server& s = server();
    if (!shouldBroadcastThisTick()) return Value(false);

    // Here the per-player encode is unavoidable: each player sees a
    // different subset. The trade is deliberate -- more CPU on the server
    // in exchange for less bandwidth per client and, more importantly, not
    // handing every client the positions of players they cannot see, which
    // is the data a wallhack is built from.
    int64_t sent = 0;
    for (auto& kv : s.players) {
        if (kv.second.markedForClose || !kv.second.session.ready) continue;
        if (sendKindTo(kv.second, net::FRAME_JSON, encodePacket(stateEnvelope(visibleTo(kv.first))))) ++sent;
    }
    return Value(sent);
}

Value SendToPlayer(const Value& player_id, const Value& data) {
    Server& s = server();
    auto it = s.players.find(static_cast<int>(player_id.asInt()));
    if (it == s.players.end() || !it->second.session.ready) return Value(false);
    return Value(sendKindTo(it->second, net::FRAME_JSON, encodePacket(data)));
}

Value SendToAllPlayers(const Value& data) {
    Server& s = server();
    std::string body = encodePacket(data);
    int64_t sent = 0;
    for (auto& kv : s.players) {
        if (kv.second.markedForClose || !kv.second.session.ready) continue;
        if (sendKindTo(kv.second, net::FRAME_JSON, body)) ++sent;
    }
    return Value(sent);
}

Value SetBroadcastFormat(const Value& format) {
    std::string f = format.asString();
    if (f != "json" && f != "msgpack" && f != "binary") {
        logLine("WARN", "PlS::SetBroadcastFormat: unknown format '" + f + "'; keeping json");
        return Value(false);
    }
    if (f != "json") {
        logLine("WARN", "PlS::SetBroadcastFormat: '" + f +
                            "' is not implemented in this runtime; JSON will be sent instead "
                            "(see runtime/README.md)");
    }
    server().broadcastFormat = f;
    return Value(true);
}

Value SetVisibilityRange(const Value& distance) {
    double d = distance.asFloat();
    if (!(d > 0.0) || !std::isfinite(d)) {
        logLine("WARN", "PlS::SetVisibilityRange: ignoring non-positive range");
        return Value(false);
    }
    server().visibilityRange = d;
    return Value(true);
}

Value SetBroadcastInterval(const Value& ticks) {
    int n = static_cast<int>(ticks.asInt());
    if (n < 1) n = 1;
    server().broadcastInterval = n;
    return Value(true);
}

// ---- Serialization helpers ------------------------------------------------

Value SerializeEntity(const Value& entity_id) {
    ServerEntity* e = findEntity(static_cast<uint64_t>(entity_id.asInt()));
    return e ? serializeEntity(*e) : Value();
}

Value SerializeAllEntities() {
    Value entities = Value::MakeObject();
    for (auto& kv : server().entities) {
        entities.objectRef()[std::to_string(kv.first)] = serializeEntity(kv.second);
    }
    return stateEnvelope(entities);
}

Value DeserializeInput(const Value& data) {
    // Inputs arriving through the normal path are already decoded by the
    // read loop, so this exists for a script holding a raw JSON string it
    // got some other way (a replay file, a test fixture). An already-decoded
    // value passes through unchanged, which makes it safe to call twice.
    if (data.type() != ValueType::String) return data;
    std::string err;
    json::Json j = json::Json::parse(data.asString(), &err);
    if (!err.empty()) {
        logLine("WARN", "PlS::DeserializeInput: malformed JSON: " + err);
        return Value();
    }
    return net::jsonToValue(j);
}

Value GetEntityData(const Value& entity_id) {
    // Same content as SerializeEntity. Kept distinct because the reference
    // lists both, and because their *intents* differ: this one is for a
    // script inspecting an entity, and is therefore free to gain
    // server-only fields later that must never go on the wire.
    ServerEntity* e = findEntity(static_cast<uint64_t>(entity_id.asInt()));
    if (!e) return Value();
    Value out = serializeEntity(*e);
    out.objectRef()["radius"] = Value(e->radius);
    out.objectRef()["is_player"] = Value(e->isPlayer);
    return out;
}

} // namespace rt
} // namespace pls
