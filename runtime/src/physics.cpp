// physics.cpp -- server-side simulation: integration, collision detection,
// raycasting, and spatial queries.
//
// --- WHY SPHERES ----------------------------------------------------------
// Every entity is a sphere here, with one radius. That is not a placeholder
// for "real" collision shapes; it is the right fidelity for this layer.
//
// An authoritative server does not need to reproduce the client's rendering
// geometry -- it needs to answer "could this player plausibly have moved
// there / hit that" cheaply and identically for every client. Sphere tests
// are exact, branch-free, order-independent, and cost one dot product;
// convex hulls or triangle meshes would multiply the per-tick cost by
// orders of magnitude and introduce solver tolerances that make a tick
// non-reproducible, which would in turn make PlS::RecordTick/ReplayTick
// useless for the debugging they exist for.
//
// Games needing precise hit geometry typically layer their own check on top
// of the broad-phase answers these commands give, which is why
// PlS::CheckCollisions returns pairs rather than resolving them.
#include "pls/pls_runtime.h"
#include "pls/pls_server.h"

#include <cmath>

namespace pls {
namespace rt {

namespace {

double dist2(const Vec3& a, const Vec3& b) {
    double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

} // namespace

Value UpdatePhysics(const Value& delta_time) {
    double dt = delta_time.asFloat();
    // A non-finite or negative dt would move every entity to a garbage
    // position in one step. This is reachable from script arithmetic
    // (`1.0 / tickrate` with tickrate accidentally 0), so it is checked
    // rather than assumed.
    if (!std::isfinite(dt) || dt <= 0.0) return Value(false);
    // Clamp the step. A server that was paused in a debugger, or descheduled
    // by a loaded host, would otherwise resume with a huge dt and teleport
    // everything through the world -- the classic "tunnelling on resume"
    // bug. Losing a little simulated time is strictly better.
    if (dt > 0.25) dt = 0.25;

    Server& s = server();
    for (auto& kv : s.entities) {
        ServerEntity& e = kv.second;
        if (e.state == "dead") continue; // dead things don't drift
        e.position.x += e.velocity.x * dt;
        e.position.y += e.velocity.y * dt;
        e.position.z += e.velocity.z * dt;
    }
    return Value(true);
}

Value CheckCollisions() {
    Server& s = server();
    s.collisionsThisTick.clear();

    // O(n^2) over pairs. Honest about its limits: fine to a few hundred
    // entities (a few thousand pair tests per tick is nothing), and the
    // point at which a spatial hash would be worth its complexity is well
    // past what this runtime targets. Each pair is visited once, so a
    // collision is reported once rather than twice.
    Value out = Value::MakeArray();
    for (auto i = s.entities.begin(); i != s.entities.end(); ++i) {
        if (i->second.state == "dead") continue;
        auto j = i;
        for (++j; j != s.entities.end(); ++j) {
            if (j->second.state == "dead") continue;
            double r = i->second.radius + j->second.radius;
            double d2 = dist2(i->second.position, j->second.position);
            if (d2 > r * r) continue;

            Collision c;
            c.a = i->first;
            c.b = j->first;
            c.distance = std::sqrt(d2);
            s.collisionsThisTick.push_back(c);

            Value entry = Value::MakeObject();
            entry.objectRef()["a"] = Value(static_cast<int64_t>(c.a));
            entry.objectRef()["b"] = Value(static_cast<int64_t>(c.b));
            entry.objectRef()["distance"] = Value(c.distance);
            out.arrayRef().push_back(entry);
        }
    }
    return out;
}

Value GetCollisionsForEntity(const Value& entity_id) {
    // Reads the results CheckCollisions cached this tick rather than
    // recomputing, so calling it once per player in a handler stays linear
    // instead of running the whole O(n^2) sweep n times.
    Server& s = server();
    uint64_t id = static_cast<uint64_t>(entity_id.asInt());
    Value out = Value::MakeArray();
    for (const Collision& c : s.collisionsThisTick) {
        if (c.a != id && c.b != id) continue;
        Value entry = Value::MakeObject();
        entry.objectRef()["other"] = Value(static_cast<int64_t>(c.a == id ? c.b : c.a));
        entry.objectRef()["distance"] = Value(c.distance);
        out.arrayRef().push_back(entry);
    }
    return out;
}

Value ApplyForce(const Value& entity_id, const Value& x, const Value& y, const Value& z) {
    ServerEntity* e = findEntity(static_cast<uint64_t>(entity_id.asInt()));
    if (!e) return Value(false);
    // Applied as an impulse straight to velocity: entities here have no
    // mass, because a server that is arbitrating movement rather than
    // simulating rigid bodies has no use for one. "Force" is the name the
    // API reference uses, so it is kept.
    double dx = x.asFloat(), dy = y.asFloat(), dz = z.asFloat();
    if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz)) return Value(false);
    e->velocity.x += dx;
    e->velocity.y += dy;
    e->velocity.z += dz;
    return Value(true);
}

Value RaycastFromTo(const Value& x1, const Value& y1, const Value& z1, const Value& x2,
                    const Value& y2, const Value& z2) {
    Vec3 from{x1.asFloat(), y1.asFloat(), z1.asFloat()};
    Vec3 to{x2.asFloat(), y2.asFloat(), z2.asFloat()};

    Vec3 dir{to.x - from.x, to.y - from.y, to.z - from.z};
    double len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);

    Value out = Value::MakeObject();
    auto& o = out.objectRef();
    o["hit"] = Value(false);
    o["entity_id"] = Value(static_cast<int64_t>(0));
    o["distance"] = Value(0.0);
    if (len <= 0.0 || !std::isfinite(len)) return out;

    dir.x /= len;
    dir.y /= len;
    dir.z /= len;

    // Ray/sphere intersection, keeping the NEAREST hit. Taking the nearest
    // (rather than the first found) is what makes a shot stop at the wall
    // in front of the target instead of passing through it -- the map order
    // of s.entities has nothing to do with spatial order.
    double bestT = len;
    uint64_t bestId = 0;
    for (auto& kv : server().entities) {
        const ServerEntity& e = kv.second;
        if (e.state == "dead") continue;

        Vec3 oc{from.x - e.position.x, from.y - e.position.y, from.z - e.position.z};
        double b = oc.x * dir.x + oc.y * dir.y + oc.z * dir.z;
        double c = oc.x * oc.x + oc.y * oc.y + oc.z * oc.z - e.radius * e.radius;
        double disc = b * b - c;
        if (disc < 0.0) continue;

        double sq = std::sqrt(disc);
        double t = -b - sq;      // near intersection
        if (t < 0.0) t = -b + sq; // origin inside the sphere: use the far one
        if (t < 0.0 || t > bestT) continue;

        bestT = t;
        bestId = e.id;
    }

    if (bestId != 0) {
        o["hit"] = Value(true);
        o["entity_id"] = Value(static_cast<int64_t>(bestId));
        o["distance"] = Value(bestT);
    }
    return out;
}

Value Distance(const Value& entity_id1, const Value& entity_id2) {
    ServerEntity* a = findEntity(static_cast<uint64_t>(entity_id1.asInt()));
    ServerEntity* b = findEntity(static_cast<uint64_t>(entity_id2.asInt()));
    if (!a || !b) return Value(-1.0); // -1 is distinguishable from a real distance
    return Value(std::sqrt(dist2(a->position, b->position)));
}

Value FindEntitiesInSphere(const Value& cx, const Value& cy, const Value& cz, const Value& radius) {
    Vec3 centre{cx.asFloat(), cy.asFloat(), cz.asFloat()};
    double r = radius.asFloat();
    Value out = Value::MakeArray();
    if (!(r > 0.0) || !std::isfinite(r)) return out;

    double r2 = r * r;
    for (auto& kv : server().entities) {
        if (dist2(centre, kv.second.position) <= r2) {
            out.arrayRef().push_back(Value(static_cast<int64_t>(kv.first)));
        }
    }
    return out;
}

} // namespace rt
} // namespace pls
