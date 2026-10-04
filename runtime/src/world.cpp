// world.cpp -- the entity store: PlS:: commands that create, destroy, and
// mutate the authoritative world state.
//
// Entities live in a std::map keyed by id. A map (rather than a vector with
// a free list) because ids are never reused -- a stale id from a client
// must resolve to "gone", not to whichever entity happens to occupy that
// slot now, which is a whole class of exploit avoided for the cost of a
// pointer chase.
#include "pls/pls_runtime.h"
#include "pls/pls_server.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace pls {

Value serializeEntity(const ServerEntity& e) {
    // The per-entity shape inside a state packet. Documented alongside its
    // consumer in PlainVulkan's network_scene.cpp -- both sides have to
    // agree on these key names.
    Value out = Value::MakeObject();
    auto& o = out.objectRef();
    o["id"] = Value(static_cast<int64_t>(e.id));
    o["name"] = Value(e.name);
    o["state"] = Value(e.state);
    o["health"] = Value(e.health);
    o["position"] = make_array({Value(e.position.x), Value(e.position.y), Value(e.position.z)});
    o["rotation"] = make_array({Value(e.rotation.x), Value(e.rotation.y), Value(e.rotation.z)});
    o["velocity"] = make_array({Value(e.velocity.x), Value(e.velocity.y), Value(e.velocity.z)});
    return out;
}

namespace rt {

Value CreateEntity(const Value& name) {
    Server& s = server();
    ServerEntity e;
    e.id = s.nextId++;
    e.name = name.asString();
    if (e.name.empty()) e.name = "Entity_" + std::to_string(e.id);
    s.entities[e.id] = e;
    return Value(static_cast<int64_t>(e.id));
}

Value DestroyEntity(const Value& entity_id) {
    Server& s = server();
    uint64_t id = static_cast<uint64_t>(entity_id.asInt());
    return Value(s.entities.erase(id) > 0);
}

Value GetEntity(const Value& entity_id) {
    ServerEntity* e = findEntity(static_cast<uint64_t>(entity_id.asInt()));
    return e ? serializeEntity(*e) : Value();
}

Value GetAllEntities() {
    Value out = Value::MakeArray();
    for (auto& kv : server().entities) {
        out.arrayRef().push_back(Value(static_cast<int64_t>(kv.first)));
    }
    return out;
}

Value SetEntityPosition(const Value& entity_id, const Value& x, const Value& y, const Value& z) {
    ServerEntity* e = findEntity(static_cast<uint64_t>(entity_id.asInt()));
    if (!e) return Value(false);
    double nx = x.asFloat();
    double ny = y.asFloat();
    double nz = z.asFloat();
    if (!std::isfinite(nx) || !std::isfinite(ny) || !std::isfinite(nz)) return Value(false);
    if (e->isPlayer && e->clampMoves && server().tickrate > 0) {
        double maxStep = server().maxPlayerSpeed / static_cast<double>(server().tickrate);
        // First spawn is often far from the default origin; allow a one-shot
        // teleport while health is still the default and the player has
        // never moved (distance from 0,0,0 with no velocity). After that,
        // clamp per-tick displacement so a compromised script cannot honour
        // a client-supplied absolute position.
        double dx = nx - e->position.x;
        double dy = ny - e->position.y;
        double dz = nz - e->position.z;
        double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist > maxStep && dist > 0.0001) {
            double s = maxStep / dist;
            nx = e->position.x + dx * s;
            ny = e->position.y + dy * s;
            nz = e->position.z + dz * s;
        }
    }
    e->position = Vec3{nx, ny, nz};
    return Value(true);
}

Value GetEntityPosition(const Value& entity_id) {
    ServerEntity* e = findEntity(static_cast<uint64_t>(entity_id.asInt()));
    if (!e) {
        // A zero vector rather than null: callers overwhelmingly index the
        // result immediately (`pos[0] + dx`), and null would make that a
        // silent zero anyway -- but via a path that also loses the array
        // shape and breaks `Len(pos)`.
        return make_array({Value(0.0), Value(0.0), Value(0.0)});
    }
    return make_array({Value(e->position.x), Value(e->position.y), Value(e->position.z)});
}

Value SetEntityRotation(const Value& entity_id, const Value& pitch, const Value& yaw,
                        const Value& roll) {
    ServerEntity* e = findEntity(static_cast<uint64_t>(entity_id.asInt()));
    if (!e) return Value(false);
    e->rotation = Vec3{pitch.asFloat(), yaw.asFloat(), roll.asFloat()};
    return Value(true);
}

Value SetEntityVelocity(const Value& entity_id, const Value& vx, const Value& vy, const Value& vz) {
    ServerEntity* e = findEntity(static_cast<uint64_t>(entity_id.asInt()));
    if (!e) return Value(false);
    e->velocity = Vec3{vx.asFloat(), vy.asFloat(), vz.asFloat()};
    return Value(true);
}

Value SetEntityHealth(const Value& entity_id, const Value& health) {
    ServerEntity* e = findEntity(static_cast<uint64_t>(entity_id.asInt()));
    if (!e) return Value(false);
    e->health = health.asFloat();
    return Value(true);
}

Value GetEntityHealth(const Value& entity_id) {
    ServerEntity* e = findEntity(static_cast<uint64_t>(entity_id.asInt()));
    return Value(e ? e->health : 0.0);
}

Value SetEntityState(const Value& entity_id, const Value& state) {
    ServerEntity* e = findEntity(static_cast<uint64_t>(entity_id.asInt()));
    if (!e) return Value(false);
    e->state = state.asString();
    return Value(true);
}

Value SetEntityRadius(const Value& entity_id, const Value& radius) {
    ServerEntity* e = findEntity(static_cast<uint64_t>(entity_id.asInt()));
    if (!e) return Value(false);
    double r = radius.asFloat();
    // A negative or zero radius would make an entity uncollidable and
    // unhittable by a raycast, which is almost never what is meant and is
    // very hard to spot from the symptom.
    if (r <= 0.0) {
        logLine("WARN", "PlS::SetEntityRadius: ignoring non-positive radius for entity " +
                            std::to_string(e->id));
        return Value(false);
    }
    e->radius = r;
    return Value(true);
}

} // namespace rt
} // namespace pls
